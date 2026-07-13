using CommunityToolkit.Maui.Storage;
using SailorEditor.Commands;
using SailorEditor.Shell;
using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class WorkspaceUiService
{
    readonly WorkspaceLifecycleService _workspaceLifecycle;
    readonly EngineService _engineService;
    readonly WorkspaceActivationCoordinator _activationCoordinator;
    readonly EditorShellHost _shellHost;
    readonly ICommandHistoryService _commandHistory;
    readonly SelectionService _selectionService;
    static readonly FilePickerFileType WorkspaceManifestFileType = new(new Dictionary<DevicePlatform, IEnumerable<string>>
    {
        { DevicePlatform.WinUI, [".sailor"] },
        { DevicePlatform.MacCatalyst, ["public.data"] },
        { DevicePlatform.iOS, ["public.data"] },
        { DevicePlatform.Android, ["*/*"] }
    });

    public WorkspaceUiService(
        WorkspaceLifecycleService workspaceLifecycle,
        EngineService engineService,
        WorkspaceActivationCoordinator activationCoordinator,
        EditorShellHost shellHost,
        ICommandHistoryService commandHistory,
        SelectionService selectionService)
    {
        _workspaceLifecycle = workspaceLifecycle;
        _engineService = engineService;
        _activationCoordinator = activationCoordinator;
        _shellHost = shellHost;
        _commandHistory = commandHistory;
        _selectionService = selectionService;
        _activationCoordinator.StateChanged += OnActivationStateChanged;
        _engineService.OnLifecycleStateChanged += OnEngineLifecycleStateChanged;
    }

    public WorkspaceUiProjection Projection { get; private set; } = WorkspaceUiProjection.Empty;

    public event EventHandler? ProjectionChanged;

    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        await RefreshAsync(cancellationToken);
        if (_engineService.State is EngineLifecycleState.Stopped or EngineLifecycleState.Faulted)
        {
            try
            {
                await _engineService.StartAsync(_engineService.GetLaunchContext(), cancellationToken);
            }
            catch (Exception ex)
            {
                ShowRepairState($"Repository runtime startup failed: {ex.Message}");
            }
        }
    }

    public async Task RefreshAsync(CancellationToken cancellationToken = default)
    {
        var recent = await _workspaceLifecycle.LoadRecentAsync(cancellationToken);
        SetProjection(WorkspaceUiProjectionBuilder.Build(
            _workspaceLifecycle.Current,
            recent,
            GetProjectedActivationState()));
    }

    public async Task NewWorkspaceAsync(CancellationToken cancellationToken = default)
    {
        var page = GetPage();
        if (page is null)
            return;

        var manifestPath = await PickWorkspaceManifestPathAsync(page, cancellationToken);
        if (string.IsNullOrWhiteSpace(manifestPath))
            return;

        var directory = Path.GetDirectoryName(Path.GetFullPath(manifestPath));
        if (string.IsNullOrWhiteSpace(directory))
        {
            await page.DisplayAlert("New Workspace", $"Workspace manifest path is invalid: {manifestPath}", "OK");
            return;
        }

        var name = Path.GetFileNameWithoutExtension(manifestPath);
        if (string.IsNullOrWhiteSpace(name))
            name = "SailorWorkspace";

        var request = new WorkspaceCreateRequest(
            name,
            directory,
            _engineService.EngineWorkingDirectory,
            ManifestPath: manifestPath);
        await ActivateAsync(
            "New Workspace",
            $"create:{manifestPath}",
            token => _workspaceLifecycle.PrepareCreateAsync(request, token),
            $"Created workspace '{name}'.",
            cancellationToken);
    }

    public async Task OpenWorkspaceAsync(CancellationToken cancellationToken = default)
    {
        var page = GetPage();
        if (page is null)
            return;

        FileResult? picked;
        try
        {
            picked = await FilePicker.Default.PickAsync(new PickOptions
            {
                PickerTitle = "Load Sailor workspace",
                FileTypes = WorkspaceManifestFileType
            });
        }
        catch (Exception ex)
        {
            await page.DisplayAlert("Load Workspace", ex.Message, "OK");
            return;
        }

        if (picked is null || string.IsNullOrWhiteSpace(picked.FullPath))
            return;

        if (!string.Equals(Path.GetExtension(picked.FullPath), ".sailor", StringComparison.OrdinalIgnoreCase))
        {
            await page.DisplayAlert("Load Workspace", "Select a .sailor workspace manifest.", "OK");
            return;
        }

        await OpenWorkspaceAsync(picked.FullPath, cancellationToken);
    }

    public async Task OpenWorkspaceAsync(RecentWorkspaceEntry recentWorkspace, CancellationToken cancellationToken = default)
    {
        await OpenWorkspaceAsync(recentWorkspace.ManifestPath, cancellationToken);
    }

    public async Task OpenWorkspaceAsync(string manifestPath, CancellationToken cancellationToken = default)
    {
        var name = Path.GetFileNameWithoutExtension(manifestPath);
        await ActivateAsync(
            "Load Workspace",
            $"open:{Path.GetFullPath(manifestPath)}",
            token => _workspaceLifecycle.PrepareOpenAsync(manifestPath, token),
            $"Loaded workspace '{name}'.",
            cancellationToken);
    }

    public async Task SaveWorkspaceAsync(CancellationToken cancellationToken = default)
    {
        var page = GetPage();
        var requestedSession = _workspaceLifecycle.Current;
        if (requestedSession is null)
        {
            if (page is not null)
                await page.DisplayAlert("Save Workspace", "No active workspace is open.", "OK");
            return;
        }

        await _activationCoordinator.RunSerializedAsync(async token =>
        {
            if (!ReferenceEquals(_workspaceLifecycle.Current, requestedSession))
            {
                await MainThread.InvokeOnMainThreadAsync(async () =>
                {
                    if (GetPage() is { } changedPage)
                    {
                        await changedPage.DisplayAlert(
                            "Save Workspace",
                            "The active workspace changed before the save could begin. Save the current workspace again.",
                            "OK");
                    }
                });
                return;
            }

            var result = await _workspaceLifecycle.SaveAsync(requestedSession, token).ConfigureAwait(false);
            var name = result.Session?.Manifest.Name ?? requestedSession.Manifest.Name;
            await MainThread.InvokeOnMainThreadAsync(
                () => ApplyLifecycleResultAsync(
                    "Save Workspace",
                    result,
                    $"Saved workspace '{name}'.",
                    CancellationToken.None));
        }, cancellationToken);
    }

    async Task ActivateAsync(
        string title,
        string requestName,
        Func<CancellationToken, Task<WorkspaceLifecyclePreparationResult>> prepareAsync,
        string successMessage,
        CancellationToken cancellationToken)
    {
        var result = await _activationCoordinator.ActivateAsync(
            new WorkspaceActivationRequest(
                requestName,
                async token => CreateCandidate(await prepareAsync(token).ConfigureAwait(false))),
            cancellationToken);

        await RefreshAsync(CancellationToken.None);
        if (!result.Succeeded)
        {
            if (result.RequiresRepair)
            {
                var message = string.IsNullOrWhiteSpace(result.Error)
                    ? "Workspace activation failed after the previous runtime stopped. Open a valid workspace or retry activation to repair the editor session."
                    : $"{result.Error}{Environment.NewLine}{Environment.NewLine}Open a valid workspace or retry activation to repair the editor session.";
                _shellHost.SetStatus("Workspace activation requires repair.");
                if (GetPage() is { } repairPage)
                    await repairPage.DisplayAlert(title, message, "OK");
            }
            else if (!result.WasCancelled && GetPage() is { } page)
            {
                await page.DisplayAlert(title, result.Error ?? "Workspace preflight failed.", "OK");
            }
            return;
        }

        var activatedName = result.Candidate?.Session.Manifest.Name;
        _shellHost.SetStatus(string.IsNullOrWhiteSpace(activatedName)
            ? successMessage
            : title == "New Workspace"
                ? $"Created workspace '{activatedName}'."
                : $"Loaded workspace '{activatedName}'.");
#if MACCATALYST
        SailorEditor.AppDelegate.RequestMenuRebuild();
#endif
    }

    WorkspaceActivationCandidate CreateCandidate(WorkspaceLifecyclePreparationResult prepared)
    {
        if (!prepared.Succeeded || prepared.Preparation is null)
            throw new InvalidOperationException(FormatError(prepared));

        var preparation = prepared.Preparation;
        try
        {
            var session = preparation.Session;
            var launchContext = EngineLaunchContract.Resolve(
                session.WorkspaceRoot,
                session.ManifestPath,
                session.ContentDirectory,
                session.CacheDirectory,
                _engineService.EngineWorkingDirectory,
                session.Manifest.WorkspaceId);
            return new WorkspaceActivationCandidate(preparation, launchContext);
        }
        catch
        {
            preparation.Discard();
            throw;
        }
    }

    async Task ApplyLifecycleResultAsync(string title, WorkspaceLifecycleResult result, string successMessage, CancellationToken cancellationToken)
    {
        var page = GetPage();
        if (!result.Succeeded)
        {
            if (page is not null)
                await page.DisplayAlert(title, FormatError(result), "OK");
            await RefreshAsync(cancellationToken);
            return;
        }

        await RefreshAsync(cancellationToken);
        if (result.Session is not null &&
            ReferenceEquals(_workspaceLifecycle.Current, result.Session))
        {
            _shellHost.SetStatus(successMessage);
        }
#if MACCATALYST
        SailorEditor.AppDelegate.RequestMenuRebuild();
#endif
    }

    void OnActivationStateChanged(object? sender, WorkspaceActivationState state)
    {
        MainThread.BeginInvokeOnMainThread(() =>
        {
            var projection = Projection with
            {
                ActivationPhase = state.Phase,
                ActivationError = state.Error
            };
            if (state.Candidate is { } candidate &&
                state.Phase is WorkspaceActivationPhase.Starting or
                    WorkspaceActivationPhase.Ready or
                    WorkspaceActivationPhase.Repair)
            {
                projection = projection with
                {
                    ActiveWorkspaceName = string.IsNullOrWhiteSpace(candidate.Session.Manifest.Name)
                        ? Path.GetFileName(candidate.Session.WorkspaceRoot)
                        : candidate.Session.Manifest.Name,
                    ActiveWorkspacePath = candidate.Session.WorkspaceRoot,
                    HasActiveWorkspace = true
                };
            }

            SetProjection(projection);
        });
    }

    void OnEngineLifecycleStateChanged(EngineLifecycleState state)
    {
        if (state != EngineLifecycleState.Faulted)
            return;

        var message = FormatEngineFailure();

        _commandHistory.BeginWorkspaceChange();
        _selectionService.BeginWorkspaceChange();
        if (!_activationCoordinator.ReportRuntimeFailure(message))
            ShowRepairState(message);
    }

    void ShowRepairState(string message)
    {
        _commandHistory.BeginWorkspaceChange();
        _selectionService.BeginWorkspaceChange();
        MainThread.BeginInvokeOnMainThread(() =>
        {
            _shellHost.SetStatus("Workspace runtime requires repair.");
            SetProjection(Projection with
            {
                ActivationPhase = WorkspaceActivationPhase.Repair,
                ActivationError = message
            });
        });
    }

    WorkspaceActivationState GetProjectedActivationState()
    {
        var activation = _activationCoordinator.State;
        if (_engineService.State == EngineLifecycleState.Faulted &&
            activation.Phase is WorkspaceActivationPhase.Idle or WorkspaceActivationPhase.Ready)
        {
            return activation with
            {
                Phase = WorkspaceActivationPhase.Repair,
                Error = FormatEngineFailure()
            };
        }

        return activation;
    }

    string FormatEngineFailure()
    {
        var failure = _engineService.LastFailure?.Message;
        var exitCode = _engineService.LastExitCode;
        return string.IsNullOrWhiteSpace(failure)
            ? $"Workspace runtime exited with code {exitCode}."
            : $"Workspace runtime failed: {failure} (exit code {exitCode}).";
    }

    void SetProjection(WorkspaceUiProjection projection)
    {
        Projection = projection;
        ProjectionChanged?.Invoke(this, EventArgs.Empty);
    }

    static async Task<string?> PickWorkspaceManifestPathAsync(Page page, CancellationToken cancellationToken)
    {
        try
        {
            using var stream = new MemoryStream();
            var result = await MainThread.InvokeOnMainThreadAsync(
                () => FileSaver.Default.SaveAsync(
                    Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                    "SailorWorkspace.sailor",
                    stream,
                    cancellationToken));
            if (result.IsCancelled)
                return null;
            if (!result.IsSuccessful)
            {
                if (result.Exception is not null)
                    await page.DisplayAlert("New Workspace", result.Exception.Message, "OK");
                return null;
            }

            if (string.IsNullOrWhiteSpace(result.FilePath))
                return null;

            var manifestPath = Path.GetFullPath(result.FilePath);
            if (!string.Equals(Path.GetExtension(manifestPath), ".sailor", StringComparison.OrdinalIgnoreCase))
            {
                await page.DisplayAlert("New Workspace", "Save the workspace manifest with a .sailor extension.", "OK");
                return null;
            }

            return manifestPath;
        }
        catch (Exception ex)
        {
            await page.DisplayAlert("New Workspace", ex.Message, "OK");
            return null;
        }
    }

    static Page? GetPage()
        => Microsoft.Maui.Controls.Shell.Current?.CurrentPage
            ?? Application.Current?.Windows.FirstOrDefault()?.Page
            ?? Application.Current?.MainPage;

    static string FormatError(WorkspaceLifecycleResult result)
    {
        if (!string.IsNullOrWhiteSpace(result.Error))
            return result.Error;

        if (result.Validation.Issues.Count > 0)
            return string.Join(Environment.NewLine, result.Validation.Issues.Select(x => $"{x.Field}: {x.Message}"));

        return "Workspace operation failed.";
    }

    static string FormatError(WorkspaceLifecyclePreparationResult result)
    {
        if (!string.IsNullOrWhiteSpace(result.Error))
            return result.Error;

        if (result.Validation.Issues.Count > 0)
            return string.Join(Environment.NewLine, result.Validation.Issues.Select(x => $"{x.Field}: {x.Message}"));

        return "Workspace preflight failed.";
    }

}
