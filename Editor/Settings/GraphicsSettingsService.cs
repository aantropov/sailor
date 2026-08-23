using System.Security.Cryptography;
using System.Text;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Settings;

public sealed record GraphicsSettingsApplyResult(
    bool ProjectChanged,
    bool QualityChanged,
    bool StatsChanged,
    bool EngineRestarted,
    bool StatsAppliedLive);

public sealed class GraphicsSettingsStaleSnapshotException : InvalidOperationException
{
    public GraphicsSettingsStaleSnapshotException(string message)
        : base(message)
    {
    }
}

public sealed class GraphicsSettingsPersistenceException : InvalidOperationException
{
    public GraphicsSettingsPersistenceException(string message)
        : base(message)
    {
    }
}

public sealed record GraphicsSettingsStorageSnapshot(
    ProjectSettingsDocument Project,
    WorkspaceEditorSettingsDocument Editor,
    GraphicsSettingsFileRevision ProjectRevision,
    GraphicsSettingsFileRevision EditorRevision,
    IReadOnlyList<string> Diagnostics);

public interface IGraphicsSettingsStorage
{
    Task<GraphicsSettingsStorageSnapshot> LoadAsync(
        GraphicsSettingsPaths paths,
        CancellationToken cancellationToken);

    Task ValidateForUpdateAsync(
        string path,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken);

    Task<GraphicsSettingsFileRevision> WriteProjectAsync(
        string path,
        ProjectSettingsDocument document,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken);

    Task<GraphicsSettingsFileRevision> WriteEditorAsync(
        string path,
        WorkspaceEditorSettingsDocument document,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken);
}

public sealed class GraphicsSettingsService
{
    readonly Func<GraphicsSettingsPaths> _pathsProvider;
    readonly Func<CancellationToken, Task<bool>> _restartEngineAsync;
    readonly Func<GraphicsStatsMode, CancellationToken, Task<bool>> _applyStatsModeAsync;
    readonly IGraphicsSettingsStorage _storage;
    readonly Func<long> _workspaceGenerationProvider;
    readonly Func<Func<CancellationToken, Task>, CancellationToken, Task>
        _serializeWorkspaceMutationAsync;
    readonly Func<long, CancellationToken, Task<bool>>
        _restartEngineForGenerationAsync;
    readonly Func<GraphicsStatsMode, long, CancellationToken, Task<bool>>
        _applyStatsModeForGenerationAsync;
    readonly SemaphoreSlim _gate = new(1, 1);
    GraphicsSettingsSnapshot? _current;

    public GraphicsSettingsService(
        Func<GraphicsSettingsPaths> pathsProvider,
        Func<CancellationToken, Task<bool>>? restartEngineAsync = null,
        Func<GraphicsStatsMode, CancellationToken, Task<bool>>? applyStatsModeAsync = null,
        IGraphicsSettingsStorage? storage = null,
        Func<long>? workspaceGenerationProvider = null,
        Func<Func<CancellationToken, Task>, CancellationToken, Task>?
            serializeWorkspaceMutationAsync = null,
        Func<long, CancellationToken, Task<bool>>?
            restartEngineForGenerationAsync = null,
        Func<GraphicsStatsMode, long, CancellationToken, Task<bool>>?
            applyStatsModeForGenerationAsync = null)
    {
        _pathsProvider = pathsProvider ?? throw new ArgumentNullException(nameof(pathsProvider));
        _restartEngineAsync = restartEngineAsync ?? (_ => Task.FromResult(true));
        _applyStatsModeAsync = applyStatsModeAsync ?? ((_, _) => Task.FromResult(true));
        _storage = storage ?? new FileGraphicsSettingsStorage();
        _workspaceGenerationProvider = workspaceGenerationProvider ?? (() => 0L);
        _serializeWorkspaceMutationAsync = serializeWorkspaceMutationAsync ??
            ((operation, cancellationToken) => operation(cancellationToken));
        _restartEngineForGenerationAsync = restartEngineForGenerationAsync ??
            ((_, cancellationToken) => _restartEngineAsync(cancellationToken));
        _applyStatsModeForGenerationAsync = applyStatsModeForGenerationAsync ??
            ((mode, _, cancellationToken) =>
                _applyStatsModeAsync(mode, cancellationToken));
    }

    public GraphicsSettingsSnapshot? Current => Volatile.Read(ref _current);

    public event EventHandler<GraphicsSettingsSnapshot>? SettingsChanged;

    public async Task<GraphicsSettingsSnapshot> EnsureLoadedAsync(
        CancellationToken cancellationToken = default)
        => await LoadForCurrentWorkspaceAsync(
                forceReload: false,
                cancellationToken)
            .ConfigureAwait(false);

    public async Task<GraphicsSettingsSnapshot> ReloadAsync(
        CancellationToken cancellationToken = default)
        => await LoadForCurrentWorkspaceAsync(
                forceReload: true,
                cancellationToken)
            .ConfigureAwait(false);

    async Task<GraphicsSettingsSnapshot> LoadForCurrentWorkspaceAsync(
        bool forceReload,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!TryCaptureWorkspace(out var workspace))
                continue;

            if (!forceReload && Current is { } cached &&
                SnapshotMatchesWorkspace(cached, workspace))
            {
                return cached;
            }

            GraphicsSettingsSnapshot? loaded = null;
            var retry = false;
            await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                if (!IsWorkspaceCurrent(workspace))
                {
                    retry = true;
                }
                else if (!forceReload && Current is { } current &&
                    SnapshotMatchesWorkspace(current, workspace))
                {
                    return current;
                }
                else
                {
                    loaded = await LoadCoreAsync(workspace, cancellationToken)
                        .ConfigureAwait(false);
                    if (!IsWorkspaceCurrent(workspace))
                    {
                        retry = true;
                        loaded = null;
                    }
                    else
                    {
                        Volatile.Write(ref _current, loaded);
                    }
                }
            }
            finally
            {
                _gate.Release();
            }

            if (retry || loaded is null)
                continue;

            if (!IsWorkspaceCurrent(workspace))
            {
                Interlocked.CompareExchange(ref _current, null, loaded);
                continue;
            }

            PublishChanged(loaded);
            return loaded;
        }
    }

    public async Task<GraphicsSettingsApplyResult> ApplyAsync(
        ProjectSettingsDocument project,
        WorkspaceEditorSettingsDocument editor,
        GraphicsSettingsSnapshot? expectedSnapshot = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(project);
        ArgumentNullException.ThrowIfNull(editor);
        ThrowIfInvalid(GraphicsSettingsValidator.Validate(project));
        ThrowIfInvalid(GraphicsSettingsValidator.Validate(editor));

        var workspace = CaptureWorkspaceOrThrow();
        if (expectedSnapshot is not null &&
            !SnapshotMatchesWorkspace(expectedSnapshot, workspace))
        {
            throw new GraphicsSettingsStaleSnapshotException(
                "The active workspace changed while graphics settings were being edited. Reload the Settings panel before applying.");
        }

        bool projectChanged;
        bool qualityChanged;
        bool statsChanged;
        GraphicsSettingsSnapshot updated;
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            ThrowIfWorkspaceChanged(workspace);
            var current = Current;
            if (current is null || !SnapshotMatchesWorkspace(current, workspace))
            {
                current = await LoadCoreAsync(workspace, cancellationToken)
                    .ConfigureAwait(false);
                ThrowIfWorkspaceChanged(workspace);
            }
            if (expectedSnapshot is not null &&
                !ReferenceEquals(current, expectedSnapshot))
            {
                throw new GraphicsSettingsStaleSnapshotException(
                    "Graphics settings changed while they were being edited. Reload before applying.");
            }

            projectChanged = !GraphicsSettingsEquality.ProjectEquals(
                current.Project,
                project);
            var selectedQualityChanged =
                current.Editor.Graphics.SelectedQuality !=
                editor.Graphics.SelectedQuality;
            statsChanged = current.Editor.Graphics.StatsMode !=
                editor.Graphics.StatsMode;
            var editorChanged = selectedQualityChanged || statsChanged;
            qualityChanged = projectChanged || selectedQualityChanged;

            GraphicsSettingsSnapshot? persisted = null;
            await _serializeWorkspaceMutationAsync(
                async mutationCancellationToken =>
                {
                    ThrowIfWorkspaceChanged(workspace);
                    await _storage.ValidateForUpdateAsync(
                            workspace.Paths.ProjectSettingsPath,
                            current.ProjectRevision,
                            mutationCancellationToken)
                        .ConfigureAwait(false);
                    ThrowIfWorkspaceChanged(workspace);
                    await _storage.ValidateForUpdateAsync(
                            workspace.Paths.EditorSettingsPath,
                            current.EditorRevision,
                            mutationCancellationToken)
                        .ConfigureAwait(false);
                    ThrowIfWorkspaceChanged(workspace);

                    var expectedProjectRevision = current.ProjectRevision;
                    var expectedEditorRevision = current.EditorRevision;
                    if (projectChanged)
                    {
                        expectedProjectRevision = await _storage.WriteProjectAsync(
                                workspace.Paths.ProjectSettingsPath,
                                project,
                                current.ProjectRevision,
                                mutationCancellationToken)
                            .ConfigureAwait(false);
                        ThrowIfWorkspaceChanged(workspace);
                    }

                    if (editorChanged)
                    {
                        expectedEditorRevision = await _storage.WriteEditorAsync(
                                workspace.Paths.EditorSettingsPath,
                                editor,
                                current.EditorRevision,
                                mutationCancellationToken)
                            .ConfigureAwait(false);
                        ThrowIfWorkspaceChanged(workspace);
                    }

                    var reloaded = projectChanged || editorChanged
                        ? await LoadCoreAsync(
                                workspace,
                                mutationCancellationToken)
                            .ConfigureAwait(false)
                        : current;
                    ThrowIfWorkspaceChanged(workspace);
                    if (reloaded.ProjectRevision != expectedProjectRevision ||
                        reloaded.EditorRevision != expectedEditorRevision ||
                        (projectChanged && !GraphicsSettingsEquality.ProjectEquals(
                            reloaded.Project,
                            project)) ||
                        (editorChanged && !GraphicsSettingsEquality.EditorEquals(
                            reloaded.Editor,
                            editor)))
                    {
                        throw new GraphicsSettingsStaleSnapshotException(
                            "A graphics settings file changed while the update was being finalized. Reload before applying again.");
                    }
                    persisted = reloaded;
                },
                cancellationToken).ConfigureAwait(false);
            updated = persisted ?? throw new InvalidOperationException(
                "The graphics settings persistence turn did not produce a snapshot.");
            Volatile.Write(ref _current, updated);
        }
        finally
        {
            _gate.Release();
        }

        if (!IsWorkspaceCurrent(workspace))
        {
            Interlocked.CompareExchange(ref _current, null, updated);
            throw new GraphicsSettingsStaleSnapshotException(
                "The active workspace changed before graphics settings could be published.");
        }
        PublishChanged(updated);

        var restarted = false;
        var statsApplied = false;
        if (qualityChanged)
        {
            ThrowIfWorkspaceChanged(workspace);
            restarted = await _restartEngineForGenerationAsync(
                    workspace.Generation,
                    cancellationToken)
                .ConfigureAwait(false);
            ThrowIfWorkspaceChanged(workspace);
            if (statsChanged && !restarted)
            {
                ThrowIfWorkspaceChanged(workspace);
                statsApplied = await _applyStatsModeForGenerationAsync(
                        editor.Graphics.StatsMode,
                        workspace.Generation,
                        cancellationToken)
                    .ConfigureAwait(false);
                ThrowIfWorkspaceChanged(workspace);
            }
        }
        else if (statsChanged)
        {
            ThrowIfWorkspaceChanged(workspace);
            statsApplied = await _applyStatsModeForGenerationAsync(
                    editor.Graphics.StatsMode,
                    workspace.Generation,
                    cancellationToken)
                .ConfigureAwait(false);
            ThrowIfWorkspaceChanged(workspace);
        }

        return new GraphicsSettingsApplyResult(
            projectChanged,
            qualityChanged,
            statsChanged,
            restarted,
            statsApplied);
    }

    public async Task<GraphicsSettingsApplyResult> SetSelectedQualityAsync(
        EditorQualitySelection selectedQuality,
        CancellationToken cancellationToken = default)
    {
        if (!Enum.IsDefined(selectedQuality))
            throw new ArgumentOutOfRangeException(nameof(selectedQuality));

        return await ApplyLatestEditorChangeAsync(
                editor => editor with
                {
                    Graphics = editor.Graphics with
                    {
                        SelectedQuality = selectedQuality
                    }
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async Task<GraphicsSettingsApplyResult> SetStatsModeAsync(
        GraphicsStatsMode statsMode,
        CancellationToken cancellationToken = default)
    {
        if (!Enum.IsDefined(statsMode))
            throw new ArgumentOutOfRangeException(nameof(statsMode));

        return await ApplyLatestEditorChangeAsync(
                editor => editor with
                {
                    Graphics = editor.Graphics with
                    {
                        StatsMode = statsMode
                    }
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    async Task<GraphicsSettingsApplyResult> ApplyLatestEditorChangeAsync(
        Func<WorkspaceEditorSettingsDocument, WorkspaceEditorSettingsDocument> update,
        CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < 3; ++attempt)
        {
            var current = await EnsureLoadedAsync(cancellationToken)
                .ConfigureAwait(false);
            try
            {
                return await ApplyAsync(
                        current.Project,
                        update(current.Editor),
                        current,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (GraphicsSettingsStaleSnapshotException) when (attempt < 2)
            {
            }
        }

        throw new GraphicsSettingsStaleSnapshotException(
            "Graphics settings kept changing while the editor update was being applied.");
    }

    GraphicsSettingsPaths ResolvePaths()
    {
        var paths = _pathsProvider() ??
            throw new InvalidOperationException(
                "The active workspace did not provide graphics settings paths.");
        return GraphicsSettingsPaths.Create(
            paths.WorkspaceRoot,
            paths.CacheDirectory);
    }

    bool TryCaptureWorkspace(out WorkspaceStamp workspace)
    {
        var generationBefore = _workspaceGenerationProvider();
        var paths = ResolvePaths();
        var generationAfter = _workspaceGenerationProvider();
        if (generationBefore != generationAfter)
        {
            workspace = default;
            return false;
        }

        workspace = new WorkspaceStamp(paths, generationAfter);
        return true;
    }

    WorkspaceStamp CaptureWorkspaceOrThrow()
    {
        if (TryCaptureWorkspace(out var workspace))
            return workspace;

        throw new GraphicsSettingsStaleSnapshotException(
            "The active workspace changed while graphics settings were being resolved. Try again.");
    }

    bool IsWorkspaceCurrent(WorkspaceStamp workspace)
        => TryCaptureWorkspace(out var current) &&
            current.Generation == workspace.Generation &&
            PathsEqual(current.Paths, workspace.Paths);

    void ThrowIfWorkspaceChanged(WorkspaceStamp workspace)
    {
        if (!IsWorkspaceCurrent(workspace))
        {
            if (Current is { } stale &&
                SnapshotMatchesWorkspace(stale, workspace))
            {
                Interlocked.CompareExchange(ref _current, null, stale);
            }
            throw new GraphicsSettingsStaleSnapshotException(
                "The active workspace changed during the graphics settings operation. Reload and try again.");
        }
    }

    static bool SnapshotMatchesWorkspace(
        GraphicsSettingsSnapshot snapshot,
        WorkspaceStamp workspace)
        => snapshot.WorkspaceGeneration == workspace.Generation &&
            PathsEqual(snapshot.Paths, workspace.Paths);

    async Task<GraphicsSettingsSnapshot> LoadCoreAsync(
        WorkspaceStamp workspace,
        CancellationToken cancellationToken)
    {
        var stored = await _storage.LoadAsync(
                workspace.Paths,
                cancellationToken)
            .ConfigureAwait(false);
        return new GraphicsSettingsSnapshot(
            workspace.Paths,
            workspace.Generation,
            stored.Project,
            stored.Editor,
            stored.ProjectRevision,
            stored.EditorRevision,
            stored.Diagnostics);
    }

    static void ThrowIfInvalid(GraphicsSettingsValidationResult validation)
    {
        if (validation.IsValid)
            return;

        throw new InvalidOperationException(string.Join(
            Environment.NewLine,
            validation.Issues.Select(issue =>
                $"{issue.Path}: {issue.Message}")));
    }

    static bool PathsEqual(
        GraphicsSettingsPaths left,
        GraphicsSettingsPaths right)
    {
        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal;
        return string.Equals(
                left.WorkspaceRoot,
                right.WorkspaceRoot,
                comparison) &&
            string.Equals(
                left.CacheDirectory,
                right.CacheDirectory,
                comparison);
    }

    void PublishChanged(GraphicsSettingsSnapshot snapshot)
        => SettingsChanged?.Invoke(this, snapshot);

    readonly record struct WorkspaceStamp(
        GraphicsSettingsPaths Paths,
        long Generation);
}

public sealed class FileGraphicsSettingsStorage : IGraphicsSettingsStorage
{
    public async Task<GraphicsSettingsStorageSnapshot> LoadAsync(
        GraphicsSettingsPaths paths,
        CancellationToken cancellationToken)
    {
        var diagnostics = new List<string>();
        var projectFile = await ReadFileAsync(
                paths.ProjectSettingsPath,
                cancellationToken)
            .ConfigureAwait(false);
        var editorFile = await ReadFileAsync(
                paths.EditorSettingsPath,
                cancellationToken)
            .ConfigureAwait(false);

        var project = projectFile.Revision.Kind switch
        {
            GraphicsSettingsFileRevisionKind.Readable =>
                GraphicsSettingsYamlCodec.ParseProject(
                    projectFile.Text!,
                    diagnostics,
                    paths.ProjectSettingsPath),
            GraphicsSettingsFileRevisionKind.Missing =>
                AddMissingProjectDiagnostic(
                    paths.ProjectSettingsPath,
                    diagnostics),
            _ => AddUnreadableProjectDiagnostic(
                paths.ProjectSettingsPath,
                projectFile.Error,
                diagnostics)
        };
        var editor = editorFile.Revision.Kind switch
        {
            GraphicsSettingsFileRevisionKind.Readable =>
                GraphicsSettingsYamlCodec.ParseEditor(
                    editorFile.Text!,
                    diagnostics,
                    paths.EditorSettingsPath),
            GraphicsSettingsFileRevisionKind.Missing =>
                AddMissingEditorDiagnostic(
                    paths.EditorSettingsPath,
                    diagnostics),
            _ => AddUnreadableEditorDiagnostic(
                paths.EditorSettingsPath,
                editorFile.Error,
                diagnostics)
        };

        return new GraphicsSettingsStorageSnapshot(
            project,
            editor,
            projectFile.Revision,
            editorFile.Revision,
            diagnostics);
    }

    public async Task ValidateForUpdateAsync(
        string path,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken)
        => _ = await ReadRootForUpdateAsync(
                path,
                expectedRevision,
                cancellationToken)
            .ConfigureAwait(false);

    public Task<GraphicsSettingsFileRevision> WriteProjectAsync(
        string path,
        ProjectSettingsDocument document,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken)
        => WritePatchedAsync(
            path,
            root => GraphicsSettingsYamlCodec.PatchProject(root, document),
            expectedRevision,
            cancellationToken);

    public Task<GraphicsSettingsFileRevision> WriteEditorAsync(
        string path,
        WorkspaceEditorSettingsDocument document,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken)
        => WritePatchedAsync(
            path,
            root => GraphicsSettingsYamlCodec.PatchEditor(root, document),
            expectedRevision,
            cancellationToken);

    static ProjectSettingsDocument AddMissingProjectDiagnostic(
        string path,
        ICollection<string> diagnostics)
    {
        diagnostics.Add(
            $"{path}: project settings file was not found; using built-in defaults.");
        return GraphicsSettingsDefaults.Project;
    }

    static ProjectSettingsDocument AddUnreadableProjectDiagnostic(
        string path,
        string? error,
        ICollection<string> diagnostics)
    {
        diagnostics.Add(
            $"{path}: failed to read project settings ({error ?? "unknown I/O error"}); using built-in defaults.");
        return GraphicsSettingsDefaults.Project;
    }

    static WorkspaceEditorSettingsDocument AddMissingEditorDiagnostic(
        string path,
        ICollection<string> diagnostics)
    {
        diagnostics.Add(
            $"{path}: workspace editor settings file was not found; using built-in defaults.");
        return GraphicsSettingsDefaults.Editor;
    }

    static WorkspaceEditorSettingsDocument AddUnreadableEditorDiagnostic(
        string path,
        string? error,
        ICollection<string> diagnostics)
    {
        diagnostics.Add(
            $"{path}: failed to read workspace editor settings ({error ?? "unknown I/O error"}); using built-in defaults.");
        return GraphicsSettingsDefaults.Editor;
    }

    static async Task<GraphicsSettingsFileRead> ReadFileAsync(
        string path,
        CancellationToken cancellationToken)
    {
        try
        {
            if (!File.Exists(path))
                return GraphicsSettingsFileRead.Missing;

            var bytes = await File.ReadAllBytesAsync(path, cancellationToken)
                .ConfigureAwait(false);
            return GraphicsSettingsFileRead.Readable(
                DecodeText(bytes),
                CalculateHash(bytes));
        }
        catch (Exception exception) when (exception is not OperationCanceledException)
        {
            if (!File.Exists(path))
                return GraphicsSettingsFileRead.Missing;
            return GraphicsSettingsFileRead.Unreadable(exception.Message);
        }
    }

    static string DecodeText(byte[] bytes)
    {
        using var stream = new MemoryStream(bytes, writable: false);
        using var reader = new StreamReader(
            stream,
            Encoding.UTF8,
            detectEncodingFromByteOrderMarks: true);
        return reader.ReadToEnd();
    }

    static string CalculateHash(ReadOnlySpan<byte> bytes)
        => Convert.ToHexString(SHA256.HashData(bytes));

    static async Task<GraphicsSettingsFileUpdate> ReadRootForUpdateAsync(
        string path,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken)
    {
        var current = await ReadFileAsync(path, cancellationToken)
            .ConfigureAwait(false);
        if (current.Revision != expectedRevision)
        {
            throw new GraphicsSettingsStaleSnapshotException(
                $"Settings file '{path}' changed outside the editor. Reload before applying.");
        }
        if (current.Revision.Kind == GraphicsSettingsFileRevisionKind.Unreadable)
        {
            throw new GraphicsSettingsPersistenceException(
                $"Settings file '{path}' cannot be read and will not be overwritten: {current.Error ?? "unknown I/O error"}.");
        }
        if (current.Revision.Kind == GraphicsSettingsFileRevisionKind.Missing)
        {
            return new GraphicsSettingsFileUpdate(
                new YamlMappingNode(),
                current.Revision);
        }

        var diagnostics = new List<string>();
        if (!GraphicsSettingsYamlCodec.TryLoadRoot(
                current.Text!,
                path,
                diagnostics,
                out var root))
        {
            throw new GraphicsSettingsPersistenceException(
                $"Settings file '{path}' is malformed and will not be overwritten: {string.Join(" ", diagnostics)}");
        }
        return new GraphicsSettingsFileUpdate(root, current.Revision);
    }

    static async Task<GraphicsSettingsFileRevision> WritePatchedAsync(
        string path,
        Action<YamlMappingNode> patch,
        GraphicsSettingsFileRevision expectedRevision,
        CancellationToken cancellationToken)
    {
        var directory = Path.GetDirectoryName(path);
        if (string.IsNullOrWhiteSpace(directory))
            throw new InvalidOperationException(
                $"Settings path does not have a parent directory: {path}");
        Directory.CreateDirectory(directory);

        var current = await ReadRootForUpdateAsync(
                path,
                expectedRevision,
                cancellationToken)
            .ConfigureAwait(false);
        patch(current.Root);
        var bytes = Encoding.UTF8.GetBytes(
            GraphicsSettingsYamlCodec.Save(current.Root));
        var writtenRevision = GraphicsSettingsFileRevision.Readable(
            CalculateHash(bytes));
        var temporaryPath = Path.Combine(
            directory,
            $".{Path.GetFileName(path)}.{Guid.NewGuid():N}.tmp");
        try
        {
            await using (var stream = new FileStream(
                temporaryPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                bufferSize: 4096,
                options: FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await stream.WriteAsync(bytes, cancellationToken)
                    .ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken)
                    .ConfigureAwait(false);
            }

            var beforeReplace = await ReadFileAsync(path, cancellationToken)
                .ConfigureAwait(false);
            if (beforeReplace.Revision != current.Revision)
            {
                throw new GraphicsSettingsStaleSnapshotException(
                    $"Settings file '{path}' changed while the replacement was being prepared. The external version was preserved.");
            }

            File.Move(temporaryPath, path, overwrite: true);
            return writtenRevision;
        }
        finally
        {
            if (File.Exists(temporaryPath))
                File.Delete(temporaryPath);
        }
    }

    sealed record GraphicsSettingsFileRead(
        GraphicsSettingsFileRevision Revision,
        string? Text,
        string? Error)
    {
        public static GraphicsSettingsFileRead Missing { get; } = new(
            GraphicsSettingsFileRevision.Missing,
            null,
            null);

        public static GraphicsSettingsFileRead Readable(
            string text,
            string hash)
            => new(
                GraphicsSettingsFileRevision.Readable(hash),
                text,
                null);

        public static GraphicsSettingsFileRead Unreadable(string error)
            => new(
                GraphicsSettingsFileRevision.Unreadable,
                null,
                error);
    }

    sealed record GraphicsSettingsFileUpdate(
        YamlMappingNode Root,
        GraphicsSettingsFileRevision Revision);
}

static class GraphicsSettingsEquality
{
    public static bool ProjectEquals(
        ProjectSettingsDocument left,
        ProjectSettingsDocument right)
    {
        if (left.SettingsVersion != right.SettingsVersion ||
            left.Graphics.DefaultQuality != right.Graphics.DefaultQuality)
        {
            return false;
        }

        foreach (var quality in Enum.GetValues<GraphicsQualityLevel>())
        {
            if (!PresetEquals(
                    left.Graphics.Presets.Get(quality),
                    right.Graphics.Presets.Get(quality)))
            {
                return false;
            }
        }

        return true;
    }

    public static bool EditorEquals(
        WorkspaceEditorSettingsDocument left,
        WorkspaceEditorSettingsDocument right)
        => left.SettingsVersion == right.SettingsVersion &&
            left.Graphics.SelectedQuality == right.Graphics.SelectedQuality &&
            left.Graphics.StatsMode == right.Graphics.StatsMode;

    static bool PresetEquals(
        GraphicsQualityPresetSettings left,
        GraphicsQualityPresetSettings right)
        => left.ResolutionFactor.Equals(right.ResolutionFactor) &&
            left.FpsCap == right.FpsCap &&
            left.MsaaSamples == right.MsaaSamples &&
            left.ShadowQuality == right.ShadowQuality &&
            left.ShadowBias.Equals(right.ShadowBias) &&
            left.ShadowCascadeCount == right.ShadowCascadeCount &&
            left.ShadowCascadeResolutions.SequenceEqual(
                right.ShadowCascadeResolutions) &&
            left.SupportSoftShadows == right.SupportSoftShadows &&
            left.CloudsResolutionMultiplier.Equals(
                right.CloudsResolutionMultiplier) &&
            left.SkyResolution == right.SkyResolution &&
            left.VegetationInstanceBudget == right.VegetationInstanceBudget &&
            left.LodBias == right.LodBias;
}
