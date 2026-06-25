using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class WorkspaceUiService
{
    readonly WorkspaceLifecycleService _workspaceLifecycle;
    readonly EngineService _engineService;

    public WorkspaceUiService(WorkspaceLifecycleService workspaceLifecycle, EngineService engineService)
    {
        _workspaceLifecycle = workspaceLifecycle;
        _engineService = engineService;
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

        var name = await page.DisplayPromptAsync(
            "New Workspace",
            "Workspace name",
            "Next",
            "Cancel",
            "Project name");
        name = name?.Trim();
        if (string.IsNullOrWhiteSpace(name))
            return;

        var defaultDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
            "Documents",
            "Sailor",
            SanitizeDirectoryName(name));

        var directory = await page.DisplayPromptAsync(
            "New Workspace",
            "Workspace directory",
            "Create",
            "Cancel",
            "Path",
            -1,
            Keyboard.Text,
            defaultDirectory);
        directory = directory?.Trim();
        if (string.IsNullOrWhiteSpace(directory))
            return;

        var result = await _workspaceLifecycle.CreateAsync(
            new WorkspaceCreateRequest(name, directory, _engineService.EngineWorkingDirectory),
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
                PickerTitle = "Open Sailor workspace"
            });
        }
        catch (Exception ex)
        {
            await page.DisplayAlert("Open Workspace", ex.Message, "OK");
            return;
        }

        if (picked is null || string.IsNullOrWhiteSpace(picked.FullPath))
            return;

        if (!string.Equals(Path.GetExtension(picked.FullPath), ".sailor", StringComparison.OrdinalIgnoreCase))
        {
            await page.DisplayAlert("Open Workspace", "Select a .sailor workspace manifest.", "OK");
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
        await ApplyResultAsync("Open Workspace", result, $"Opened workspace '{name}'.", cancellationToken);
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
        if (page is not null)
            await page.DisplayAlert(title, successMessage, "OK");
    }

    void SetProjection(WorkspaceUiProjection projection)
    {
        Projection = projection;
        ProjectionChanged?.Invoke(this, EventArgs.Empty);
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

    static string SanitizeDirectoryName(string name)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var sanitized = new string(name.Select(x => invalid.Contains(x) ? '-' : x).ToArray()).Trim();
        return string.IsNullOrWhiteSpace(sanitized) ? "SailorWorkspace" : sanitized;
    }
}
