using CommunityToolkit.Maui.Storage;
using SailorEditor.Content;
using SailorEditor.Shell;
using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class WorkspaceUiService
{
    readonly WorkspaceLifecycleService _workspaceLifecycle;
    readonly EngineService _engineService;
    readonly AssetsService _assetsService;
    static readonly FilePickerFileType WorkspaceManifestFileType = new(new Dictionary<DevicePlatform, IEnumerable<string>>
    {
        { DevicePlatform.WinUI, [".sailor"] },
        { DevicePlatform.MacCatalyst, ["public.data"] },
        { DevicePlatform.iOS, ["public.data"] },
        { DevicePlatform.Android, ["*/*"] }
    });

    public WorkspaceUiService(WorkspaceLifecycleService workspaceLifecycle, EngineService engineService, AssetsService assetsService)
    {
        _workspaceLifecycle = workspaceLifecycle;
        _engineService = engineService;
        _assetsService = assetsService;
    }

    public WorkspaceUiProjection Projection { get; private set; } = WorkspaceUiProjection.Empty;

    public event EventHandler? ProjectionChanged;

    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        await RefreshAsync(cancellationToken);
    }

    public async Task RefreshAsync(CancellationToken cancellationToken = default)
    {
        var recent = await _workspaceLifecycle.LoadRecentAsync(cancellationToken);
        SetProjection(WorkspaceUiProjectionBuilder.Build(_workspaceLifecycle.Current, recent));
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

        var result = await _workspaceLifecycle.CreateAsync(
            new WorkspaceCreateRequest(name, directory, _engineService.EngineWorkingDirectory, ManifestPath: manifestPath),
            cancellationToken);

        await ApplyResultAsync("New Workspace", result, $"Created workspace '{name}'.", cancellationToken);
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
        var result = await _workspaceLifecycle.OpenAsync(manifestPath, cancellationToken);
        var name = result.Session?.Manifest.Name ?? Path.GetFileNameWithoutExtension(manifestPath);
        await ApplyResultAsync("Load Workspace", result, $"Loaded workspace '{name}'.", cancellationToken);
    }

    public async Task SaveWorkspaceAsync(CancellationToken cancellationToken = default)
    {
        var page = GetPage();
        if (_workspaceLifecycle.Current is null)
        {
            if (page is not null)
                await page.DisplayAlert("Save Workspace", "No active workspace is open.", "OK");
            return;
        }

        var result = await _workspaceLifecycle.SaveAsync(_workspaceLifecycle.Current, cancellationToken);
        var name = result.Session?.Manifest.Name ?? _workspaceLifecycle.Current.Manifest.Name;
        await ApplyResultAsync("Save Workspace", result, $"Saved workspace '{name}'.", cancellationToken);
    }

    async Task ApplyResultAsync(string title, WorkspaceLifecycleResult result, string successMessage, CancellationToken cancellationToken)
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
        if (result.Session is not null)
        {
            if (!ProjectContentPathPolicy.IsSamePath(_assetsService.CurrentProjectRootPath, result.Session.ContentDirectory))
                MauiProgram.GetService<SelectionService>().ClearSelection();
            _assetsService.AddProjectRoot(result.Session.ContentDirectory);
        }
        MauiProgram.GetService<EditorShellHost>().SetStatus(successMessage);
#if MACCATALYST
        SailorEditor.AppDelegate.RequestMenuRebuild();
#endif
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

}
