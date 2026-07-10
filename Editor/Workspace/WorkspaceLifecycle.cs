using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.Workspace;

public sealed record WorkspaceCreateRequest(
    string Name,
    string WorkspaceDirectory,
    string EnginePath,
    string? WorkspaceId = null,
    string? ManifestPath = null,
    string EngineReferenceKind = WorkspaceEngineReferenceKinds.Source);

public sealed record WorkspaceSession(
    string WorkspaceRoot,
    string ManifestPath,
    WorkspaceManifest Manifest,
    string ContentDirectory,
    string SourceDirectory,
    string GeneratedProjectDirectory,
    string CacheDirectory)
{
    public string BuildDirectory { get; init; } = string.Empty;
    public string LogicOutputDirectory { get; init; } = string.Empty;
}

public sealed record WorkspaceLifecycleResult(
    WorkspaceSession? Session,
    WorkspaceManifestValidationResult Validation,
    string? Error = null)
{
    public bool Succeeded => Session is not null && Validation.IsValid && string.IsNullOrEmpty(Error);
}

public sealed record RecentWorkspaceEntry
{
    public RecentWorkspaceEntry()
    {
    }

    public RecentWorkspaceEntry(string workspaceId, string name, string manifestPath, DateTimeOffset lastOpenedAt)
    {
        WorkspaceId = workspaceId;
        Name = name;
        ManifestPath = manifestPath;
        LastOpenedAt = lastOpenedAt;
    }

    public string WorkspaceId { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string ManifestPath { get; init; } = string.Empty;
    public DateTimeOffset LastOpenedAt { get; init; }
}

public class WorkspaceTemplateService
{
    public const string ManifestFileName = "workspace.sailor";

    readonly WorkspaceManifestSerializer _serializer;
    readonly WorkspaceProjectGenerator _projectGenerator;

    public WorkspaceTemplateService(WorkspaceManifestSerializer serializer)
        : this(serializer, new WorkspaceProjectGenerator())
    {
    }

    public WorkspaceTemplateService(
        WorkspaceManifestSerializer serializer,
        WorkspaceProjectGenerator projectGenerator)
    {
        _serializer = serializer;
        _projectGenerator = projectGenerator;
    }

    public virtual async Task<WorkspaceSession> CreateAsync(WorkspaceCreateRequest request, CancellationToken cancellationToken = default)
    {
        var workspaceRoot = Path.GetFullPath(request.WorkspaceDirectory);
        var manifestPath = string.IsNullOrWhiteSpace(request.ManifestPath)
            ? Path.Combine(workspaceRoot, ManifestFileName)
            : Path.GetFullPath(request.ManifestPath);

        EnsureInsideWorkspace(workspaceRoot, manifestPath, nameof(WorkspaceCreateRequest.ManifestPath));
        ValidateCleanWorkspaceDirectory(workspaceRoot, manifestPath);
        Directory.CreateDirectory(workspaceRoot);

        var manifest = WorkspaceManifest.CreateDefault(
            request.Name,
            request.EnginePath,
            request.WorkspaceId,
            request.EngineReferenceKind);
        var session = CreateSession(workspaceRoot, manifest, manifestPath);

        EnsureInsideWorkspace(workspaceRoot, session.ContentDirectory, nameof(WorkspaceManifest.ContentPath));
        EnsureInsideWorkspace(workspaceRoot, session.SourceDirectory, nameof(WorkspaceManifest.SourcePath));
        EnsureInsideWorkspace(workspaceRoot, session.GeneratedProjectDirectory, nameof(WorkspaceManifest.GeneratedProjectPath));
        EnsureInsideWorkspace(workspaceRoot, session.CacheDirectory, nameof(WorkspaceManifest.CachePath));
        EnsureInsideWorkspace(workspaceRoot, session.BuildDirectory, nameof(WorkspaceManifest.BuildPath));
        EnsureInsideWorkspace(workspaceRoot, session.LogicOutputDirectory, nameof(WorkspaceManifest.LogicOutputPath));

        Directory.CreateDirectory(session.ContentDirectory);
        Directory.CreateDirectory(session.SourceDirectory);
        Directory.CreateDirectory(session.GeneratedProjectDirectory);
        Directory.CreateDirectory(session.CacheDirectory);
        Directory.CreateDirectory(session.BuildDirectory);
        Directory.CreateDirectory(session.LogicOutputDirectory);

        await _projectGenerator.GenerateAsync(session, cancellationToken);
        await _serializer.SaveAsync(session.ManifestPath, manifest, cancellationToken);
        return session;
    }

    public WorkspaceSession CreateSession(string workspaceRoot, WorkspaceManifest manifest, string? manifestPath = null)
    {
        var root = Path.GetFullPath(workspaceRoot);
        var resolvedManifestPath = Path.GetFullPath(manifestPath ?? Path.Combine(root, ManifestFileName));
        EnsureInsideWorkspace(root, resolvedManifestPath, nameof(WorkspaceSession.ManifestPath));

        return new WorkspaceSession(
            root,
            resolvedManifestPath,
            manifest,
            ResolveWorkspacePath(root, manifest.ContentPath),
            ResolveWorkspacePath(root, manifest.SourcePath),
            ResolveWorkspacePath(root, manifest.GeneratedProjectPath),
            ResolveWorkspacePath(root, manifest.CachePath))
        {
            BuildDirectory = ResolveWorkspacePath(root, manifest.BuildPath),
            LogicOutputDirectory = ResolveWorkspacePath(root, manifest.LogicOutputPath)
        };
    }

    static void ValidateCleanWorkspaceDirectory(string workspaceRoot, string allowedManifestPath)
    {
        if (!Directory.Exists(workspaceRoot))
            return;

        var allowed = NormalizePath(allowedManifestPath);
        if (Directory.EnumerateFileSystemEntries(workspaceRoot).Any(x => !string.Equals(NormalizePath(x), allowed, PathComparison)))
            throw new InvalidOperationException($"Workspace directory is not empty: {workspaceRoot}");
    }

    static string NormalizePath(string path)
        => Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

    static string ResolveWorkspacePath(string workspaceRoot, string relativePath)
    {
        var path = relativePath
            .Replace('/', Path.DirectorySeparatorChar)
            .Replace('\\', Path.DirectorySeparatorChar);

        var fullPath = Path.GetFullPath(Path.Combine(workspaceRoot, path));
        EnsureInsideWorkspace(workspaceRoot, fullPath, relativePath);
        return fullPath;
    }

    static void EnsureInsideWorkspace(string workspaceRoot, string path, string field)
    {
        var root = Path.GetFullPath(workspaceRoot).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var fullPath = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        if (string.Equals(root, fullPath, PathComparison))
            return;

        var prefix = root + Path.DirectorySeparatorChar;
        if (!fullPath.StartsWith(prefix, PathComparison))
            throw new InvalidOperationException($"{field} resolves outside the workspace root.");
    }

    static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}

public sealed class RecentWorkspaceStore
{
    const int DefaultMaxEntries = 20;

    readonly string _path;
    readonly Func<DateTimeOffset> _clock;
    readonly int _maxEntries;
    readonly ISerializer _serializer = new SerializerBuilder()
        .WithNamingConvention(CamelCaseNamingConvention.Instance)
        .Build();
    readonly IDeserializer _deserializer = new DeserializerBuilder()
        .WithNamingConvention(CamelCaseNamingConvention.Instance)
        .IgnoreUnmatchedProperties()
        .Build();

    public RecentWorkspaceStore(string? path = null, Func<DateTimeOffset>? clock = null, int maxEntries = DefaultMaxEntries)
    {
        var appDataDirectory = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        _path = path ?? Path.Combine(appDataDirectory, "SailorEditor", "Workspaces", "recent-workspaces.yaml");
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _maxEntries = maxEntries;
    }

    public async Task<IReadOnlyList<RecentWorkspaceEntry>> LoadAsync(CancellationToken cancellationToken = default)
    {
        if (!File.Exists(_path))
            return [];

        try
        {
            var yaml = await File.ReadAllTextAsync(_path, cancellationToken);
            var entries = _deserializer.Deserialize<List<RecentWorkspaceEntry>>(yaml) ?? [];
            return Normalize(entries);
        }
        catch
        {
            return [];
        }
    }

    public IReadOnlyList<RecentWorkspaceEntry> Load()
    {
        if (!File.Exists(_path))
            return [];

        try
        {
            var yaml = File.ReadAllText(_path);
            var entries = _deserializer.Deserialize<List<RecentWorkspaceEntry>>(yaml) ?? [];
            return Normalize(entries);
        }
        catch
        {
            return [];
        }
    }

    public async Task SaveAsync(IReadOnlyList<RecentWorkspaceEntry> entries, CancellationToken cancellationToken = default)
    {
        var directory = Path.GetDirectoryName(_path);
        if (!string.IsNullOrWhiteSpace(directory))
            Directory.CreateDirectory(directory);

        await File.WriteAllTextAsync(_path, _serializer.Serialize(Normalize(entries)), cancellationToken);
    }

    public async Task<IReadOnlyList<RecentWorkspaceEntry>> RecordAsync(WorkspaceSession session, CancellationToken cancellationToken = default)
    {
        var entries = await LoadAsync(cancellationToken);
        var updated = new RecentWorkspaceEntry(
            session.Manifest.WorkspaceId,
            session.Manifest.Name,
            Path.GetFullPath(session.ManifestPath),
            _clock());

        var normalizedPath = NormalizePath(updated.ManifestPath);
        var next = entries
            .Where(x => !string.Equals(NormalizePath(x.ManifestPath), normalizedPath, StringComparison.OrdinalIgnoreCase))
            .Prepend(updated)
            .ToArray();

        await SaveAsync(next, cancellationToken);
        return Normalize(next);
    }

    IReadOnlyList<RecentWorkspaceEntry> Normalize(IEnumerable<RecentWorkspaceEntry> entries)
        => entries
            .Where(x => !string.IsNullOrWhiteSpace(x.ManifestPath))
            .Select(TryNormalize)
            .Where(x => x is not null)
            .Select(x => x!)
            .GroupBy(x => NormalizePath(x.ManifestPath), StringComparer.OrdinalIgnoreCase)
            .Select(x => x.OrderByDescending(entry => entry.LastOpenedAt).First())
            .OrderByDescending(x => x.LastOpenedAt)
            .Take(_maxEntries)
            .ToArray();

    static RecentWorkspaceEntry? TryNormalize(RecentWorkspaceEntry entry)
    {
        try
        {
            return entry with { ManifestPath = Path.GetFullPath(entry.ManifestPath) };
        }
        catch
        {
            return null;
        }
    }

    static string NormalizePath(string path) => Path.GetFullPath(path)
        .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
}

public sealed class WorkspaceLifecycleService
{
    readonly WorkspaceManifestSerializer _serializer;
    readonly WorkspaceTemplateService _templateService;
    readonly RecentWorkspaceStore _recentWorkspaceStore;

    public WorkspaceLifecycleService(
        WorkspaceManifestSerializer serializer,
        WorkspaceTemplateService templateService,
        RecentWorkspaceStore recentWorkspaceStore)
    {
        _serializer = serializer;
        _templateService = templateService;
        _recentWorkspaceStore = recentWorkspaceStore;
    }

    public WorkspaceSession? Current { get; private set; }

    public Task<IReadOnlyList<RecentWorkspaceEntry>> LoadRecentAsync(CancellationToken cancellationToken = default)
        => _recentWorkspaceStore.LoadAsync(cancellationToken);

    public async Task<WorkspaceLifecycleResult> CreateAsync(WorkspaceCreateRequest request, CancellationToken cancellationToken = default)
    {
        try
        {
            var session = await _templateService.CreateAsync(request, cancellationToken);
            Current = session;
            await _recentWorkspaceStore.RecordAsync(session, cancellationToken);
            return new WorkspaceLifecycleResult(session, WorkspaceManifestValidationResult.Success);
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecycleResult(null, WorkspaceManifestValidationResult.Success, ex.Message);
        }
    }

    public async Task<WorkspaceLifecycleResult> OpenAsync(string manifestPath, CancellationToken cancellationToken = default)
    {
        var loadResult = await _serializer.LoadAsync(manifestPath, cancellationToken);
        if (!loadResult.Succeeded || loadResult.Manifest is null)
            return new WorkspaceLifecycleResult(null, loadResult.Validation, loadResult.Error);

        try
        {
            var workspaceRoot = Path.GetDirectoryName(Path.GetFullPath(manifestPath));
            if (string.IsNullOrWhiteSpace(workspaceRoot))
                return new WorkspaceLifecycleResult(null, WorkspaceManifestValidationResult.Success, $"Workspace manifest path is invalid: {manifestPath}");

            var session = _templateService.CreateSession(workspaceRoot, loadResult.Manifest, manifestPath);
            Current = session;
            await _recentWorkspaceStore.RecordAsync(session, cancellationToken);
            return new WorkspaceLifecycleResult(session, loadResult.Validation);
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecycleResult(null, loadResult.Validation, ex.Message);
        }
    }

    public async Task<WorkspaceLifecycleResult> SaveAsync(WorkspaceSession session, CancellationToken cancellationToken = default)
    {
        try
        {
            var validated = _templateService.CreateSession(session.WorkspaceRoot, session.Manifest, session.ManifestPath);
            await _serializer.SaveAsync(validated.ManifestPath, validated.Manifest, cancellationToken);
            Current = validated;
            await _recentWorkspaceStore.RecordAsync(validated, cancellationToken);
            return new WorkspaceLifecycleResult(validated, WorkspaceManifestValidationResult.Success);
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecycleResult(null, WorkspaceManifestValidationResult.Success, ex.Message);
        }
    }
}
