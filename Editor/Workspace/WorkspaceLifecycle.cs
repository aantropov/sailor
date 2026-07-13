#nullable enable annotations

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

public sealed record WorkspaceLifecyclePreparationResult(
    WorkspaceLifecyclePreparation? Preparation,
    WorkspaceManifestValidationResult Validation,
    string? Error = null)
{
    public bool Succeeded => Preparation is not null && Validation.IsValid && string.IsNullOrEmpty(Error);
}

public sealed class WorkspaceLifecyclePreparation : IDisposable
{
    readonly Action _commitRecovery;
    readonly Action _rollbackRecovery;
    int _completed;

    internal WorkspaceLifecyclePreparation(
        WorkspaceSession session,
        Action? commitRecovery = null,
        Action? rollbackRecovery = null)
    {
        Session = session;
        _commitRecovery = commitRecovery ?? (() => { });
        _rollbackRecovery = rollbackRecovery ?? (() => { });
    }

    public WorkspaceSession Session { get; }

    internal void CommitRecovery()
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0)
            throw new InvalidOperationException("Workspace preparation has already been completed.");

        _commitRecovery();
    }

    public void Discard()
    {
        if (Interlocked.Exchange(ref _completed, 1) == 0)
            _rollbackRecovery();
    }

    public void Dispose() => Discard();
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
        var manifest = WorkspaceManifest.CreateDefault(
            request.Name,
            request.EnginePath,
            request.WorkspaceId,
            request.EngineReferenceKind);
        var validation = _serializer.Validate(manifest);
        if (!validation.IsValid)
        {
            throw new InvalidOperationException(
                $"Workspace manifest is invalid: {string.Join("; ", validation.Issues.Select(x => x.Message))}");
        }

        WorkspacePathPolicy.EnsureInsideRoot(workspaceRoot, manifestPath, nameof(WorkspaceCreateRequest.ManifestPath));
        ValidateCleanWorkspaceDirectory(workspaceRoot, manifestPath);
        var session = CreateSession(workspaceRoot, manifest, manifestPath);

        var manifestPlaceholderExisted = File.Exists(manifestPath) || Directory.Exists(manifestPath);
        var manifestPlaceholderContents = File.Exists(manifestPath)
            ? await File.ReadAllBytesAsync(manifestPath, CancellationToken.None)
            : null;
        WorkspaceProjectGenerationResult? generatedProject = null;

        try
        {
            generatedProject = await _projectGenerator.GenerateAsync(session, cancellationToken);
            await _serializer.SaveAsync(session.ManifestPath, manifest, cancellationToken);
            return session;
        }
        catch (Exception creationError)
        {
            try
            {
                RollbackWorkspaceCreation(
                    workspaceRoot,
                    manifestPath,
                    manifestPlaceholderExisted,
                    manifestPlaceholderContents,
                    generatedProject);
            }
            catch (Exception rollbackError)
            {
                throw new AggregateException("Workspace creation failed and could not be rolled back.", creationError, rollbackError);
            }

            throw;
        }
    }

    public WorkspaceSession CreateSession(string workspaceRoot, WorkspaceManifest manifest, string? manifestPath = null)
    {
        var validation = _serializer.Validate(manifest);
        if (!validation.IsValid)
        {
            throw new InvalidOperationException(
                $"Workspace manifest is invalid: {string.Join("; ", validation.Issues.Select(x => x.Message))}");
        }

        var root = WorkspacePathPolicy.NormalizePhysicalPath(workspaceRoot);
        var resolvedManifestPath = WorkspacePathPolicy.NormalizePhysicalPath(
            manifestPath ?? Path.Combine(root, ManifestFileName));
        WorkspacePathPolicy.EnsureInsideRoot(root, resolvedManifestPath, nameof(WorkspaceSession.ManifestPath));

        return new WorkspaceSession(
            root,
            resolvedManifestPath,
            manifest,
            WorkspacePathPolicy.ResolveOwnedPath(root, manifest.ContentPath, nameof(WorkspaceManifest.ContentPath)),
            WorkspacePathPolicy.ResolveOwnedPath(root, manifest.SourcePath, nameof(WorkspaceManifest.SourcePath)),
            WorkspacePathPolicy.ResolveOwnedPath(root, manifest.GeneratedProjectPath, nameof(WorkspaceManifest.GeneratedProjectPath)),
            WorkspacePathPolicy.ResolveOwnedPath(root, manifest.CachePath, nameof(WorkspaceManifest.CachePath)))
        {
            BuildDirectory = WorkspacePathPolicy.ResolveOwnedPath(root, manifest.BuildPath, nameof(WorkspaceManifest.BuildPath)),
            LogicOutputDirectory = WorkspacePathPolicy.ResolveOwnedPath(root, manifest.LogicOutputPath, nameof(WorkspaceManifest.LogicOutputPath))
        };
    }

    static void ValidateCleanWorkspaceDirectory(string workspaceRoot, string allowedManifestPath)
    {
        if (!Directory.Exists(workspaceRoot))
            return;

        var allowed = NormalizePath(allowedManifestPath);
        if (Directory.EnumerateFileSystemEntries(workspaceRoot).Any(x =>
            !string.Equals(NormalizePath(x), allowed, WorkspacePathPolicy.PathComparison)))
            throw new InvalidOperationException($"Workspace directory is not empty: {workspaceRoot}");
    }

    static void RollbackWorkspaceCreation(
        string workspaceRoot,
        string manifestPath,
        bool manifestPlaceholderExisted,
        byte[]? manifestPlaceholderContents,
        WorkspaceProjectGenerationResult? generatedProject)
    {
        if (!Directory.Exists(workspaceRoot))
            return;

        if (!manifestPlaceholderExisted && File.Exists(manifestPath))
        {
            File.Delete(manifestPath);
        }
        else if (manifestPlaceholderContents is not null &&
            (!File.Exists(manifestPath) || !File.ReadAllBytes(manifestPath).SequenceEqual(manifestPlaceholderContents)))
        {
            File.WriteAllBytes(manifestPath, manifestPlaceholderContents);
        }

        if (generatedProject is not null)
            WorkspaceProjectGenerator.Rollback(generatedProject);
    }

    static string NormalizePath(string path)
        => Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

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
            var prepared = await PrepareCreateAsync(request, cancellationToken);
            return await CommitPreparedAsync(prepared, cancellationToken);
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecycleResult(
                null,
                WorkspaceManifestValidationResult.Success,
                ex.Message);
        }
    }

    public async Task<WorkspaceLifecycleResult> OpenAsync(string manifestPath, CancellationToken cancellationToken = default)
    {
        var prepared = await PrepareOpenAsync(manifestPath, cancellationToken);
        return await CommitPreparedAsync(prepared, cancellationToken);
    }

    public async Task<WorkspaceLifecyclePreparationResult> PrepareCreateAsync(
        WorkspaceCreateRequest request,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var session = await _templateService.CreateAsync(request, cancellationToken);
            return new WorkspaceLifecyclePreparationResult(
                new WorkspaceLifecyclePreparation(session),
                WorkspaceManifestValidationResult.Success);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecyclePreparationResult(
                null,
                WorkspaceManifestValidationResult.Success,
                ex.Message);
        }
    }

    public async Task<WorkspaceLifecyclePreparationResult> PrepareOpenAsync(
        string manifestPath,
        CancellationToken cancellationToken = default)
    {
        WorkspaceManifestLoadResult loadResult;
        try
        {
            loadResult = await _serializer.LoadAsync(manifestPath, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecyclePreparationResult(
                null,
                WorkspaceManifestValidationResult.Success,
                ex.Message);
        }

        if (!loadResult.Succeeded || loadResult.Manifest is null)
            return new WorkspaceLifecyclePreparationResult(null, loadResult.Validation, loadResult.Error);

        WorkspaceDirectoryRecovery? recovery = null;
        try
        {
            var workspaceRoot = Path.GetDirectoryName(Path.GetFullPath(manifestPath));
            if (string.IsNullOrWhiteSpace(workspaceRoot))
            {
                return new WorkspaceLifecyclePreparationResult(
                    null,
                    WorkspaceManifestValidationResult.Success,
                    $"Workspace manifest path is invalid: {manifestPath}");
            }

            var session = _templateService.CreateSession(workspaceRoot, loadResult.Manifest, manifestPath);
            recovery = RecoverDefaultDirectories(session);
            session = recovery.Session;
            return new WorkspaceLifecyclePreparationResult(
                new WorkspaceLifecyclePreparation(session, recovery.Commit, recovery.Rollback),
                loadResult.Validation);
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecyclePreparationResult(
                null,
                loadResult.Validation,
                RollbackRecovery(recovery, ex).Message);
        }
    }

    public async Task<WorkspaceLifecycleResult> CommitPreparedAsync(
        WorkspaceLifecyclePreparationResult prepared,
        CancellationToken cancellationToken = default)
    {
        if (!prepared.Succeeded || prepared.Preparation is null)
            return new WorkspaceLifecycleResult(null, prepared.Validation, prepared.Error);

        var preparation = prepared.Preparation;
        try
        {
            await _recentWorkspaceStore.RecordAsync(preparation.Session, cancellationToken);
            preparation.CommitRecovery();
            Current = preparation.Session;
            return new WorkspaceLifecycleResult(preparation.Session, prepared.Validation);
        }
        catch (Exception ex)
        {
            try
            {
                preparation.Discard();
                return new WorkspaceLifecycleResult(null, prepared.Validation, ex.Message);
            }
            catch (Exception rollbackError)
            {
                return new WorkspaceLifecycleResult(
                    null,
                    prepared.Validation,
                    new AggregateException(
                        "Workspace commit failed and preparation could not be rolled back.",
                        ex,
                        rollbackError).Message);
            }
        }
    }

    public async Task CommitActivationAsync(
        WorkspaceLifecyclePreparation preparation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(preparation);

        // Activation has already crossed the native shutdown boundary. Publish the
        // candidate before updating secondary recents state so a storage failure
        // cannot make the stopped previous workspace look active again.
        preparation.CommitRecovery();
        Current = preparation.Session;
        await _recentWorkspaceStore.RecordAsync(preparation.Session, cancellationToken);
    }

    public async Task<WorkspaceLifecycleResult> SaveAsync(WorkspaceSession session, CancellationToken cancellationToken = default)
    {
        WorkspaceDirectoryRecovery? recovery = null;
        try
        {
            var validated = _templateService.CreateSession(session.WorkspaceRoot, session.Manifest, session.ManifestPath);
            recovery = RecoverDefaultDirectories(validated);
            validated = recovery.Session;
            await _serializer.SaveAsync(validated.ManifestPath, validated.Manifest, cancellationToken);
            await _recentWorkspaceStore.RecordAsync(validated, cancellationToken);
            recovery.Commit();
            Current = validated;
            return new WorkspaceLifecycleResult(validated, WorkspaceManifestValidationResult.Success);
        }
        catch (Exception ex)
        {
            return new WorkspaceLifecycleResult(
                null,
                WorkspaceManifestValidationResult.Success,
                RollbackRecovery(recovery, ex).Message);
        }
    }

    WorkspaceDirectoryRecovery RecoverDefaultDirectories(WorkspaceSession session)
    {
        var directoriesToCreate = new List<string>();
        ValidateRecoverableDirectory(
            session.Manifest.ContentPath,
            "Content",
            session.ContentDirectory,
            nameof(WorkspaceManifest.ContentPath),
            allowCustomCreation: false,
            directoriesToCreate);
        ValidateRecoverableDirectory(
            session.Manifest.CachePath,
            "Cache",
            session.CacheDirectory,
            nameof(WorkspaceManifest.CachePath),
            allowCustomCreation: true,
            directoriesToCreate);

        var createdDirectories = new List<string>();
        var recovery = new WorkspaceDirectoryRecovery(session, createdDirectories);
        try
        {
            foreach (var directory in directoriesToCreate)
            {
                var missingDirectories = GetMissingDirectories(session.WorkspaceRoot, directory);
                createdDirectories.AddRange(missingDirectories);
                Directory.CreateDirectory(directory);
            }

            var resolved = _templateService.CreateSession(
                session.WorkspaceRoot,
                session.Manifest,
                session.ManifestPath);
            if (!Directory.Exists(resolved.ContentDirectory))
                throw new DirectoryNotFoundException($"ContentPath was not recovered: {resolved.ContentDirectory}");
            if (!Directory.Exists(resolved.CacheDirectory))
                throw new DirectoryNotFoundException($"CachePath was not recovered: {resolved.CacheDirectory}");

            recovery.SetSession(resolved);
            return recovery;
        }
        catch (Exception ex)
        {
            throw RollbackRecovery(recovery, ex);
        }
    }

    static Exception RollbackRecovery(WorkspaceDirectoryRecovery? recovery, Exception error)
    {
        if (recovery is null)
            return error;

        try
        {
            recovery.Rollback();
            return error;
        }
        catch (Exception rollbackError)
        {
            return new AggregateException(
                "Workspace operation failed and recovered directories could not be rolled back.",
                error,
                rollbackError);
        }
    }

    static void ValidateRecoverableDirectory(
        string configuredPath,
        string defaultPath,
        string resolvedPath,
        string field,
        bool allowCustomCreation,
        ICollection<string> directoriesToCreate)
    {
        if (Directory.Exists(resolvedPath))
            return;

        if (File.Exists(resolvedPath))
            throw new InvalidOperationException($"{field} resolves to a file instead of a directory: {resolvedPath}");

        var normalizedConfiguredPath = WorkspaceManifestPaths.Normalize(configuredPath);
        if (!allowCustomCreation &&
            !string.Equals(normalizedConfiguredPath, defaultPath, WorkspacePathPolicy.PathComparison))
        {
            throw new InvalidOperationException(
                $"The custom {field} directory does not exist and will not be created automatically: {resolvedPath}");
        }

        directoriesToCreate.Add(resolvedPath);
    }

    static IReadOnlyList<string> GetMissingDirectories(string workspaceRoot, string directory)
    {
        var missing = new List<string>();
        var root = Path.GetFullPath(workspaceRoot)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        for (var current = Path.GetFullPath(directory)
                .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            !string.Equals(current, root, WorkspacePathPolicy.PathComparison) &&
            !Directory.Exists(current) &&
            !File.Exists(current);
            current = Path.GetDirectoryName(current) ?? root)
        {
            missing.Add(current);
        }

        missing.Reverse();
        return missing;
    }

    sealed class WorkspaceDirectoryRecovery
    {
        readonly IReadOnlyList<string> _createdDirectories;
        bool _completed;

        public WorkspaceDirectoryRecovery(
            WorkspaceSession session,
            IReadOnlyList<string> createdDirectories)
        {
            Session = session;
            _createdDirectories = createdDirectories;
        }

        public WorkspaceSession Session { get; private set; }

        public void SetSession(WorkspaceSession session) => Session = session;

        public void Commit() => _completed = true;

        public void Rollback()
        {
            if (_completed)
                return;

            foreach (var directory in _createdDirectories
                .Distinct(WorkspacePathPolicy.PathComparer)
                .Reverse())
            {
                if (!WorkspacePathPolicy.IsInsideRoot(Session.WorkspaceRoot, directory))
                    continue;
                if (Directory.Exists(directory) && !Directory.EnumerateFileSystemEntries(directory).Any())
                    Directory.Delete(directory);
            }

            _completed = true;
        }
    }
}
