#nullable enable

using System.Globalization;
using System.Text;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Services;

internal enum AssetCacheIndexStatus
{
    Missing,
    Loaded,
    Corrupt,
    UnsupportedVersion,
    IoFailure
}

internal sealed record AssetCacheIndexEntry(
    string FileId,
    string SourcePath,
    string MetadataFilename,
    string AssetInfoType);

internal sealed record AssetCacheIndexLoadResult(
    AssetCacheIndexStatus Status,
    string Diagnostic,
    IReadOnlyList<AssetCacheIndexEntry>? Entries = null)
{
    public bool Succeeded => Status == AssetCacheIndexStatus.Loaded && Entries is not null;
}

internal sealed class AssetCacheIndexStore
{
    internal const int CurrentCacheVersion = 1;
    internal const int CurrentPayloadVersion = 1;
    internal const string CacheKind = "asset-cache";
    internal const string ProducerIdentity = "asset-cache-v1";

    static readonly string[] EnvelopeFields =
    [
        "cacheVersion",
        "payloadVersion",
        "cacheKind",
        "workspaceId",
        "engineVersion",
        "buildIdentity",
        "producerIdentity",
        "payload"
    ];

    static readonly string[] EntryFields =
    [
        "fileId",
        "assetImportTime",
        "sourcePath",
        "sourceRevision",
        "metadataFilename",
        "metadataRevision",
        "assetInfoType"
    ];

    static readonly UTF8Encoding Utf8WithoutBom = new(false, true);

    public AssetCacheIndexLoadResult Load(
        string cacheFilePath,
        string expectedWorkspaceId,
        CancellationToken cancellationToken = default)
    {
        var fullPath = Path.GetFullPath(cacheFilePath);
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            var yaml = File.ReadAllText(fullPath, Utf8WithoutBom);
            return Parse(fullPath, yaml, expectedWorkspaceId, cancellationToken);
        }
        catch (FileNotFoundException)
        {
            return Missing(fullPath);
        }
        catch (DirectoryNotFoundException)
        {
            return Missing(fullPath);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (DecoderFallbackException exception)
        {
            return Corrupt(fullPath, $"The file is not valid UTF-8: {exception.Message}");
        }
        catch (Exception exception)
        {
            return new AssetCacheIndexLoadResult(
                AssetCacheIndexStatus.IoFailure,
                $"Cannot read asset cache index '{fullPath}': {exception.Message}");
        }
    }

    internal static AssetCacheIndexLoadResult Parse(
        string cacheFilePath,
        string yaml,
        string expectedWorkspaceId,
        CancellationToken cancellationToken = default)
    {
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            var envelope = ReadStrictFields(
                RequireMapping(LoadSingleDocument(yaml).RootNode, "Asset cache envelope"),
                "Asset cache envelope");

            if (!TryReadVersion(envelope, "cacheVersion", out var cacheVersion) ||
                cacheVersion != CurrentCacheVersion)
            {
                return Unsupported(
                    cacheFilePath,
                    $"cacheVersion must be {CurrentCacheVersion}.");
            }
            if (!TryReadVersion(envelope, "payloadVersion", out var payloadVersion) ||
                payloadVersion != CurrentPayloadVersion)
            {
                return Unsupported(
                    cacheFilePath,
                    $"payloadVersion must be {CurrentPayloadVersion}.");
            }

            RequireExactFields(envelope, EnvelopeFields, "Asset cache envelope");
            RequireScalar(envelope, "cacheKind", CacheKind);
            RequireScalar(envelope, "producerIdentity", ProducerIdentity);
            RequireScalar(envelope, "workspaceId", expectedWorkspaceId);
            _ = RequireScalar(envelope, "engineVersion");
            _ = RequireScalar(envelope, "buildIdentity");
            var payload = RequireScalar(envelope, "payload");

            var payloadRoot = ReadStrictFields(
                RequireMapping(LoadSingleDocument(payload).RootNode, "Asset cache payload"),
                "Asset cache payload");
            RequireExactFields(payloadRoot, ["assetCache"], "Asset cache payload");
            var cacheRoot = ReadStrictFields(
                RequireMapping(payloadRoot["assetCache"], "assetCache"),
                "assetCache");
            RequireExactFields(cacheRoot, ["assets"], "assetCache");
            var assets = RequireMapping(cacheRoot["assets"], "assetCache.assets");

            var entries = new List<AssetCacheIndexEntry>(assets.Children.Count);
            var fileIds = new HashSet<string>(StringComparer.Ordinal);
            foreach (var pair in assets.Children)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var mapFileId = RequireScalar(pair.Key, "assetCache.assets key");
                var fields = ReadStrictFields(
                    RequireMapping(pair.Value, $"assetCache.assets.{mapFileId}"),
                    $"assetCache.assets.{mapFileId}");
                RequireExactFields(fields, EntryFields, $"assetCache.assets.{mapFileId}");

                var fileId = RequireScalar(fields, "fileId");
                var sourcePath = RequireScalar(fields, "sourcePath");
                var metadataFilename = RequireScalar(fields, "metadataFilename");
                var assetInfoType = RequireScalar(fields, "assetInfoType");
                _ = RequireScalar(fields, "assetImportTime");
                ValidateRevision(fields["sourceRevision"], "sourceRevision");
                ValidateRevision(fields["metadataRevision"], "metadataRevision");

                if (!string.Equals(fileId, mapFileId, StringComparison.Ordinal) ||
                    !fileIds.Add(fileId))
                {
                    throw new InvalidDataException(
                        $"Asset cache entry '{mapFileId}' has a duplicate or mismatched fileId.");
                }
                if (!string.Equals(
                        Path.GetFileName(metadataFilename),
                        metadataFilename,
                        StringComparison.Ordinal))
                {
                    throw new InvalidDataException(
                        $"Asset cache entry '{mapFileId}' metadataFilename must be a filename.");
                }

                entries.Add(new AssetCacheIndexEntry(
                    fileId,
                    Path.GetFullPath(sourcePath),
                    metadataFilename,
                    assetInfoType));
            }

            return new AssetCacheIndexLoadResult(
                AssetCacheIndexStatus.Loaded,
                $"Loaded {entries.Count.ToString(CultureInfo.InvariantCulture)} asset cache index entries from '{cacheFilePath}'.",
                entries);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception) when (exception is YamlException or InvalidDataException or IOException or ArgumentException or NotSupportedException)
        {
            return Corrupt(cacheFilePath, exception.Message);
        }
    }

    static void ValidateRevision(YamlNode node, string fieldName)
    {
        var fields = ReadStrictFields(RequireMapping(node, fieldName), fieldName);
        RequireExactFields(
            fields,
            ["modificationTimeNanoseconds", "fileSize", "contentHash"],
            fieldName);
        _ = RequireScalar(fields, "modificationTimeNanoseconds");
        _ = RequireScalar(fields, "fileSize");
        _ = RequireScalar(fields, "contentHash");
    }

    static YamlDocument LoadSingleDocument(string yaml)
    {
        if (string.IsNullOrWhiteSpace(yaml))
        {
            throw new InvalidDataException("The YAML document is empty.");
        }

        var stream = new YamlStream();
        using var reader = new StringReader(yaml);
        stream.Load(reader);
        if (stream.Documents.Count != 1)
        {
            throw new InvalidDataException("The YAML input must contain exactly one document.");
        }
        return stream.Documents[0];
    }

    static YamlMappingNode RequireMapping(YamlNode node, string name)
        => node as YamlMappingNode
            ?? throw new InvalidDataException($"{name} must be a mapping.");

    static Dictionary<string, YamlNode> ReadStrictFields(
        YamlMappingNode mapping,
        string name)
    {
        var result = new Dictionary<string, YamlNode>(StringComparer.Ordinal);
        foreach (var pair in mapping.Children)
        {
            var fieldName = RequireScalar(pair.Key, $"{name} field name");
            if (!result.TryAdd(fieldName, pair.Value))
            {
                throw new InvalidDataException($"{name} contains duplicate field '{fieldName}'.");
            }
        }
        return result;
    }

    static void RequireExactFields(
        IReadOnlyDictionary<string, YamlNode> fields,
        IReadOnlyCollection<string> requiredFields,
        string name)
    {
        foreach (var requiredField in requiredFields)
        {
            if (!fields.ContainsKey(requiredField))
            {
                throw new InvalidDataException($"{name} is missing required field '{requiredField}'.");
            }
        }
        if (fields.Count != requiredFields.Count)
        {
            var unknown = fields.Keys
                .Except(requiredFields, StringComparer.Ordinal)
                .OrderBy(value => value, StringComparer.Ordinal)
                .First();
            throw new InvalidDataException($"{name} contains unknown field '{unknown}'.");
        }
    }

    static bool TryReadVersion(
        IReadOnlyDictionary<string, YamlNode> fields,
        string fieldName,
        out int version)
    {
        version = 0;
        return fields.TryGetValue(fieldName, out var node) &&
            int.TryParse(
                (node as YamlScalarNode)?.Value,
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out version);
    }

    static string RequireScalar(
        IReadOnlyDictionary<string, YamlNode> fields,
        string fieldName,
        string? expectedValue = null)
    {
        if (!fields.TryGetValue(fieldName, out var node))
        {
            throw new InvalidDataException($"Required field '{fieldName}' is missing.");
        }
        var value = RequireScalar(node, fieldName);
        if (expectedValue is not null &&
            !string.Equals(value, expectedValue, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Field '{fieldName}' must be '{expectedValue}', found '{value}'.");
        }
        return value;
    }

    static string RequireScalar(YamlNode node, string name)
    {
        if (node is not YamlScalarNode scalar || string.IsNullOrWhiteSpace(scalar.Value))
        {
            throw new InvalidDataException($"{name} must be a non-empty scalar.");
        }
        return scalar.Value;
    }

    static AssetCacheIndexLoadResult Missing(string cacheFilePath)
        => new(
            AssetCacheIndexStatus.Missing,
            $"Asset cache index is missing: '{cacheFilePath}'.");

    static AssetCacheIndexLoadResult Unsupported(string cacheFilePath, string reason)
        => new(
            AssetCacheIndexStatus.UnsupportedVersion,
            $"Asset cache index '{cacheFilePath}' is unsupported: {reason}");

    static AssetCacheIndexLoadResult Corrupt(string cacheFilePath, string reason)
        => new(
            AssetCacheIndexStatus.Corrupt,
            $"Asset cache index '{cacheFilePath}' is corrupt: {reason}");
}
