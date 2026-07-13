#nullable enable

using System.Globalization;
using System.Text;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Services;

internal enum EditorTypeCacheStatus
{
    Missing,
    Loaded,
    StaleIdentity,
    Corrupt,
    UnsupportedVersion,
    IoFailure
}

internal sealed record EditorTypeCacheIdentity(
    string WorkspaceIdentity,
    string EngineVersion,
    string BuildIdentity,
    string ProducerIdentity);

internal sealed record EditorTypeCacheLoadResult(
    EditorTypeCacheStatus Status,
    string Diagnostic,
    string? Payload = null)
{
    public bool Succeeded => Status == EditorTypeCacheStatus.Loaded && Payload is not null;
}

internal sealed record EditorTypeCacheWriteResult(
    bool Succeeded,
    string Diagnostic);

internal interface IEditorTypeCacheFileOperations
{
    string ReadAllText(string path);
    void CreateDirectory(string path);
    void WriteAllTextDurably(string path, string contents);
    void Replace(string temporaryPath, string targetPath);
    void DeleteIfExists(string path);
    IEnumerable<string> EnumerateFiles(string directory);
}

internal sealed class PhysicalEditorTypeCacheFileOperations : IEditorTypeCacheFileOperations
{
    static readonly UTF8Encoding Utf8WithoutBom = new(false, true);

    public string ReadAllText(string path) => File.ReadAllText(path, Utf8WithoutBom);

    public void CreateDirectory(string path) => Directory.CreateDirectory(path);

    public void WriteAllTextDurably(string path, string contents)
    {
        using var stream = new FileStream(
            path,
            new FileStreamOptions
            {
                Mode = FileMode.CreateNew,
                Access = FileAccess.Write,
                Share = FileShare.None,
                Options = FileOptions.WriteThrough
            });
        using (var writer = new StreamWriter(stream, Utf8WithoutBom, bufferSize: 4096, leaveOpen: true))
        {
            writer.Write(contents);
            writer.Flush();
        }

        stream.Flush(flushToDisk: true);
    }

    public void Replace(string temporaryPath, string targetPath)
        => File.Move(temporaryPath, targetPath, overwrite: true);

    public void DeleteIfExists(string path)
    {
        File.Delete(path);
    }

    public IEnumerable<string> EnumerateFiles(string directory)
    {
        try
        {
            return Directory.EnumerateFiles(directory).ToArray();
        }
        catch (DirectoryNotFoundException)
        {
            return [];
        }
    }
}

internal sealed class EditorTypeCacheStore
{
    internal const int CurrentCacheVersion = 1;
    internal const int CurrentPayloadVersion = 1;
    internal const string EditorTypesCacheKind = "editor-types";

    public static bool ShouldInvalidate(EditorTypeCacheStatus status)
        => status is EditorTypeCacheStatus.StaleIdentity
            or EditorTypeCacheStatus.Corrupt
            or EditorTypeCacheStatus.UnsupportedVersion;

    public static bool ShouldPersistLiveCatalog(EditorTypeCacheStatus loadStatus)
        => loadStatus != EditorTypeCacheStatus.IoFailure;

    static readonly string[] RequiredFields =
    [
        "cacheVersion",
        "cacheKind",
        "workspaceId",
        "engineVersion",
        "buildIdentity",
        "producerIdentity",
        "payloadVersion",
        "payload"
    ];

    static readonly string[] NativeIdentityFields =
    [
        "workspaceIdentity",
        "engineVersion",
        "buildIdentity",
        "producerIdentity"
    ];

    readonly IEditorTypeCacheFileOperations _files;

    public EditorTypeCacheStore()
        : this(new PhysicalEditorTypeCacheFileOperations())
    {
    }

    internal EditorTypeCacheStore(IEditorTypeCacheFileOperations files)
    {
        _files = files ?? throw new ArgumentNullException(nameof(files));
    }

    public static EditorTypeCacheIdentity ParseNativeIdentity(string yaml)
    {
        var document = LoadSingleDocument(yaml);
        var root = document.RootNode as YamlMappingNode
            ?? throw new InvalidDataException("The native workspace cache identity root must be a mapping.");
        var fields = ReadStrictFields(root);
        foreach (var field in NativeIdentityFields)
        {
            if (!fields.TryGetValue(field, out var value) || !TryReadRequiredScalar(value, out _))
                throw new InvalidDataException($"Native workspace cache identity field '{field}' must be a non-empty scalar.");
        }
        if (fields.Count != NativeIdentityFields.Length)
        {
            var unknown = fields.Keys.Except(NativeIdentityFields, StringComparer.Ordinal).Order().First();
            throw new InvalidDataException($"Unknown native workspace cache identity field '{unknown}'.");
        }

        return new EditorTypeCacheIdentity(
            ((YamlScalarNode)fields["workspaceIdentity"]).Value!,
            ((YamlScalarNode)fields["engineVersion"]).Value!,
            ((YamlScalarNode)fields["buildIdentity"]).Value!,
            ((YamlScalarNode)fields["producerIdentity"]).Value!);
    }

    public EditorTypeCacheLoadResult Load(
        string cacheFilePath,
        EditorTypeCacheIdentity expectedIdentity)
    {
        var fullPath = NormalizePath(cacheFilePath);
        ValidateIdentity(expectedIdentity);

        try
        {
            var yaml = _files.ReadAllText(fullPath);
            return Parse(fullPath, yaml, expectedIdentity);
        }
        catch (FileNotFoundException)
        {
            return Missing(fullPath);
        }
        catch (DirectoryNotFoundException)
        {
            return Missing(fullPath);
        }
        catch (DecoderFallbackException ex)
        {
            return Corrupt(fullPath, $"The file is not valid UTF-8: {ex.Message}");
        }
        catch (Exception ex)
        {
            return new EditorTypeCacheLoadResult(
                EditorTypeCacheStatus.IoFailure,
                $"Cannot read editor type cache '{fullPath}': {ex.Message}");
        }
    }

    public EditorTypeCacheWriteResult Save(
        string cacheFilePath,
        EditorTypeCacheIdentity identity,
        string payload)
    {
        var fullPath = NormalizePath(cacheFilePath);
        ValidateIdentity(identity);

        if (!TryValidatePayload(payload, out var payloadError))
        {
            return new EditorTypeCacheWriteResult(
                false,
                $"Cannot save editor type cache '{fullPath}': payload is corrupt: {payloadError}");
        }

        string? temporaryPath = null;
        Exception? writeFailure = null;
        Exception? cleanupFailure = null;
        try
        {
            var directory = Path.GetDirectoryName(fullPath)
                ?? throw new InvalidOperationException("The cache path does not have a parent directory.");
            _files.CreateDirectory(directory);

            temporaryPath = Path.Combine(
                directory,
                $".{Path.GetFileName(fullPath)}.{Guid.NewGuid():N}.tmp");
            var envelope = Serialize(identity, payload);
            _files.WriteAllTextDurably(temporaryPath, envelope);
            _files.Replace(temporaryPath, fullPath);
        }
        catch (Exception ex)
        {
            writeFailure = ex;
        }
        finally
        {
            if (temporaryPath is not null)
            {
                try
                {
                    _files.DeleteIfExists(temporaryPath);
                }
                catch (Exception ex)
                {
                    cleanupFailure = ex;
                }
            }
        }

        if (writeFailure is null && cleanupFailure is null)
        {
            return new EditorTypeCacheWriteResult(
                true,
                $"Saved editor type cache: '{fullPath}'.");
        }

        var diagnostic = writeFailure is null
            ? $"Cannot clean editor type cache temporary file for '{fullPath}': {cleanupFailure!.Message}"
            : $"Cannot atomically save editor type cache '{fullPath}': {writeFailure.Message}";
        if (writeFailure is not null && cleanupFailure is not null)
        {
            diagnostic += $" Temporary-file cleanup also failed: {cleanupFailure.Message}";
        }

        return new EditorTypeCacheWriteResult(false, diagnostic);
    }

    public EditorTypeCacheWriteResult Invalidate(string cacheFilePath)
    {
        var fullPath = NormalizePath(cacheFilePath);
        var directory = Path.GetDirectoryName(fullPath)
            ?? throw new InvalidOperationException("The cache path does not have a parent directory.");
        var failures = new List<string>();

        try
        {
            _files.DeleteIfExists(fullPath);
        }
        catch (Exception ex)
        {
            failures.Add($"target deletion failed: {ex.Message}");
        }

        try
        {
            foreach (var candidate in _files.EnumerateFiles(directory))
            {
                if (!IsOwnedTemporaryFile(fullPath, candidate))
                    continue;

                try
                {
                    _files.DeleteIfExists(candidate);
                }
                catch (Exception ex)
                {
                    failures.Add($"temporary-file deletion failed for '{candidate}': {ex.Message}");
                }
            }
        }
        catch (Exception ex)
        {
            failures.Add($"temporary-file discovery failed: {ex.Message}");
        }

        return failures.Count == 0
            ? new EditorTypeCacheWriteResult(
                true,
                $"Invalidated editor type cache: '{fullPath}'.")
            : new EditorTypeCacheWriteResult(
                false,
                $"Cannot completely invalidate editor type cache '{fullPath}': {string.Join("; ", failures)}");
    }

    static EditorTypeCacheLoadResult Parse(
        string cacheFilePath,
        string yaml,
        EditorTypeCacheIdentity expectedIdentity)
    {
        YamlMappingNode root;
        Dictionary<string, YamlNode> fields;
        try
        {
            var document = LoadSingleDocument(yaml);
            root = document.RootNode as YamlMappingNode
                ?? throw new InvalidDataException("The envelope root must be a mapping.");
            fields = ReadStrictFields(root);
        }
        catch (Exception ex)
        {
            return Corrupt(cacheFilePath, ex.Message);
        }

        if (!fields.TryGetValue("cacheVersion", out var cacheVersionNode))
        {
            return Unsupported(
                cacheFilePath,
                "cacheVersion is missing; raw and unversioned caches are not supported.");
        }

        if (!TryReadVersion(cacheVersionNode, out var cacheVersion))
            return Corrupt(cacheFilePath, "cacheVersion must be an integer scalar.");
        if (cacheVersion != CurrentCacheVersion)
        {
            return Unsupported(
                cacheFilePath,
                $"cacheVersion {cacheVersion} is unsupported; expected {CurrentCacheVersion}.");
        }

        if (!fields.TryGetValue("payloadVersion", out var payloadVersionNode))
        {
            return Unsupported(
                cacheFilePath,
                "payloadVersion is missing; unversioned payloads are not supported.");
        }

        if (!TryReadVersion(payloadVersionNode, out var payloadVersion))
            return Corrupt(cacheFilePath, "payloadVersion must be an integer scalar.");
        if (payloadVersion != CurrentPayloadVersion)
        {
            return Unsupported(
                cacheFilePath,
                $"payloadVersion {payloadVersion} is unsupported; expected {CurrentPayloadVersion}.");
        }

        foreach (var field in RequiredFields)
        {
            if (!fields.ContainsKey(field))
                return Corrupt(cacheFilePath, $"Required field '{field}' is missing.");
        }

        if (fields.Count != RequiredFields.Length)
        {
            var unknown = fields.Keys.Except(RequiredFields, StringComparer.Ordinal).Order().First();
            return Corrupt(cacheFilePath, $"Unknown field '{unknown}' is not allowed by cacheVersion {CurrentCacheVersion}.");
        }

        var expectedFields = new (string Name, string Value)[]
        {
            ("cacheKind", EditorTypesCacheKind),
            ("workspaceId", expectedIdentity.WorkspaceIdentity),
            ("engineVersion", expectedIdentity.EngineVersion),
            ("buildIdentity", expectedIdentity.BuildIdentity),
            ("producerIdentity", expectedIdentity.ProducerIdentity)
        };
        foreach (var (name, expected) in expectedFields)
        {
            if (!TryReadRequiredScalar(fields[name], out var actual))
                return Corrupt(cacheFilePath, $"Field '{name}' must be a non-empty scalar.");
            if (!string.Equals(actual, expected, StringComparison.Ordinal))
            {
                return new EditorTypeCacheLoadResult(
                    EditorTypeCacheStatus.StaleIdentity,
                    $"Editor type cache '{cacheFilePath}' has stale {name}: expected '{expected}', found '{actual}'.");
            }
        }

        if (!TryReadRequiredScalar(fields["payload"], out var payload))
            return Corrupt(cacheFilePath, "Field 'payload' must be a non-empty scalar.");
        if (!TryValidatePayload(payload, out var payloadError))
            return Corrupt(cacheFilePath, $"Payload is corrupt: {payloadError}");

        return new EditorTypeCacheLoadResult(
            EditorTypeCacheStatus.Loaded,
            $"Loaded editor type cache: '{cacheFilePath}'.",
            payload);
    }

    static Dictionary<string, YamlNode> ReadStrictFields(YamlMappingNode root)
    {
        var result = new Dictionary<string, YamlNode>(StringComparer.Ordinal);
        foreach (var pair in root.Children)
        {
            if (pair.Key is not YamlScalarNode key || string.IsNullOrWhiteSpace(key.Value))
                throw new InvalidDataException("Envelope field names must be non-empty scalars.");
            if (!result.TryAdd(key.Value, pair.Value))
                throw new InvalidDataException($"Envelope field '{key.Value}' is duplicated.");
        }

        return result;
    }

    static YamlDocument LoadSingleDocument(string yaml)
    {
        if (string.IsNullOrWhiteSpace(yaml))
            throw new InvalidDataException("The YAML document is empty.");

        var stream = new YamlStream();
        using var reader = new StringReader(yaml);
        stream.Load(reader);
        if (stream.Documents.Count != 1)
            throw new InvalidDataException("Exactly one YAML document is required.");
        return stream.Documents[0];
    }

    static bool TryValidatePayload(string payload, out string error)
    {
        try
        {
            var document = LoadSingleDocument(payload);
            if (document.RootNode is not YamlMappingNode)
                throw new InvalidDataException("The type-catalog root must be a mapping.");

            error = string.Empty;
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    static string Serialize(EditorTypeCacheIdentity identity, string payload)
    {
        var root = new YamlMappingNode
        {
            { "cacheVersion", CurrentCacheVersion.ToString(CultureInfo.InvariantCulture) },
            { "cacheKind", EditorTypesCacheKind },
            { "workspaceId", identity.WorkspaceIdentity },
            { "engineVersion", identity.EngineVersion },
            { "buildIdentity", identity.BuildIdentity },
            { "producerIdentity", identity.ProducerIdentity },
            { "payloadVersion", CurrentPayloadVersion.ToString(CultureInfo.InvariantCulture) },
            { new YamlScalarNode("payload"), new YamlScalarNode(payload) { Style = ScalarStyle.Literal } }
        };
        var stream = new YamlStream(new YamlDocument(root));
        using var writer = new StringWriter(CultureInfo.InvariantCulture);
        stream.Save(writer, assignAnchors: false);
        return writer.ToString();
    }

    static bool TryReadVersion(YamlNode node, out int version)
    {
        version = 0;
        return node is YamlScalarNode scalar &&
            int.TryParse(
                scalar.Value,
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out version);
    }

    static bool TryReadRequiredScalar(YamlNode node, out string value)
    {
        if (node is YamlScalarNode scalar && !string.IsNullOrWhiteSpace(scalar.Value))
        {
            value = scalar.Value;
            return true;
        }

        value = string.Empty;
        return false;
    }

    static bool IsOwnedTemporaryFile(string targetPath, string candidatePath)
    {
        var targetDirectory = Path.GetDirectoryName(targetPath);
        var candidateDirectory = Path.GetDirectoryName(Path.GetFullPath(candidatePath));
        if (!string.Equals(targetDirectory, candidateDirectory, PathComparison))
            return false;

        var filename = Path.GetFileName(candidatePath);
        var prefix = $".{Path.GetFileName(targetPath)}.";
        const string suffix = ".tmp";
        if (!filename.StartsWith(prefix, StringComparison.Ordinal) ||
            !filename.EndsWith(suffix, StringComparison.Ordinal))
        {
            return false;
        }

        var token = filename[prefix.Length..^suffix.Length];
        return token.Length == 32 && token.All(Uri.IsHexDigit);
    }

    static EditorTypeCacheLoadResult Unsupported(string path, string detail)
        => new(
            EditorTypeCacheStatus.UnsupportedVersion,
            $"Editor type cache '{path}' has an unsupported format: {detail}");

    static EditorTypeCacheLoadResult Corrupt(string path, string detail)
        => new(
            EditorTypeCacheStatus.Corrupt,
            $"Editor type cache '{path}' is corrupt: {detail}");

    static EditorTypeCacheLoadResult Missing(string path)
        => new(
            EditorTypeCacheStatus.Missing,
            $"Editor type cache is missing: '{path}'.");

    static string NormalizePath(string cacheFilePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(cacheFilePath);
        return Path.GetFullPath(cacheFilePath);
    }

    static void ValidateIdentity(EditorTypeCacheIdentity identity)
    {
        ArgumentNullException.ThrowIfNull(identity);
        ArgumentException.ThrowIfNullOrWhiteSpace(identity.WorkspaceIdentity);
        ArgumentException.ThrowIfNullOrWhiteSpace(identity.EngineVersion);
        ArgumentException.ThrowIfNullOrWhiteSpace(identity.BuildIdentity);
        ArgumentException.ThrowIfNullOrWhiteSpace(identity.ProducerIdentity);
    }

    static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}
