#if MACCATALYST
using SailorEditor.Platforms.MacCatalyst;
#endif
using SailorEditor.Services;
using SailorEditor.Shell;
using SailorEditor.Workspace;
using SailorEditor.Mcp;
using System.ComponentModel;

namespace SailorEditor;

public partial class MainPage : ContentPage
{
    readonly EditorShellHost _shellHost;
    readonly WorkspaceUiService _workspaceUi;
    readonly McpEditorHostService _mcpHost;
    bool _workspaceUiInitialized;
    bool _commandLineWorkspaceHandled;

    public MainPage(EditorShellHost shellHost)
    {
        _shellHost = shellHost;
        _workspaceUi = MauiProgram.GetService<WorkspaceUiService>();
        _mcpHost = MauiProgram.GetService<McpEditorHostService>();
        InitializeComponent();
#if MACCATALYST
        ToolbarHost.IsVisible = false;
        ToolbarHost.HeightRequest = 0;
#endif
        ShellLayoutHost.Host = _shellHost;
        _shellHost.PropertyChanged += OnShellHostPropertyChanged;
        _workspaceUi.ProjectionChanged += OnWorkspaceProjectionChanged;
        UpdateWindowTitle();
    }

    protected override async void OnAppearing()
    {
        base.OnAppearing();
        if (!_workspaceUiInitialized)
        {
            _workspaceUiInitialized = true;
            await _workspaceUi.InitializeAsync();
        }

        if (!_commandLineWorkspaceHandled)
        {
            _commandLineWorkspaceHandled = true;
            var manifestPath = ResolveCommandLineWorkspaceManifest(
                Environment.GetCommandLineArgs());
            if (!string.IsNullOrWhiteSpace(manifestPath))
            {
                await _workspaceUi.OpenWorkspaceAsync(manifestPath);
                await LoadCommandLineWorldAsync(Environment.GetCommandLineArgs());
            }
        }

        if (!_mcpHost.Status.IsRunning)
        {
            try
            {
                await _mcpHost.StartAsync();
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine(
                    "Unable to start Sailor Editor MCP host: " +
                    exception.Message);
            }
        }
        if (_shellHost.CurrentLayout is null)
            await _shellHost.InitializeAsync();
        UpdateStatusText();
    }

    void OnShellHostPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(EditorShellHost.StatusText))
            MainThread.BeginInvokeOnMainThread(UpdateStatusText);
    }

    void OnWorkspaceProjectionChanged(object? sender, EventArgs e)
    {
        MainThread.BeginInvokeOnMainThread(() =>
        {
            UpdateStatusText();
            UpdateWindowTitle();
        });
    }

    void UpdateStatusText()
    {
        var projection = _workspaceUi.Projection;
        var projectName = projection.Mode == EditorProjectMode.Workspace
            ? $"{projection.ModeLabel}: {projection.ActiveWorkspaceName}"
            : projection.ModeLabel;
        var workspaceStatus = string.IsNullOrWhiteSpace(projection.ActiveRootPath)
            ? $"{_shellHost.StatusText} • {projectName}"
            : $"{_shellHost.StatusText} • {projectName} • Root: {projection.ActiveRootPath}";
        var statusText = projection.RequiresRepair
            ? $"{workspaceStatus} • Repair required: {projection.ActivationError}"
            : projection.IsActivationInProgress
                ? $"{workspaceStatus} • Workspace activation: {projection.ActivationPhase}"
                : workspaceStatus;
        if (projection.HasGeneratedProjectAttention)
            statusText = $"{statusText} • Generated project: {projection.GeneratedProjectAttention}";
        StatusTextLabel.Text = statusText;
    }

    void UpdateWindowTitle()
    {
        var title = BuildWindowTitle(_workspaceUi.Projection);
        Title = title;
        if (Application.Current?.Windows.FirstOrDefault() is { } window)
            window.Title = title;
#if MACCATALYST
        MacCatalystWindowChrome.SetTitle(title);
#endif
    }

    static string BuildWindowTitle(WorkspaceUiProjection projection)
        => projection.WindowTitle;

    async Task LoadCommandLineWorldAsync(IReadOnlyList<string> arguments)
    {
        var world = ResolveCommandLineValue(arguments, "--world");
        var contentRoot = _workspaceUi.Projection.ActiveContentPath;
        if (string.IsNullOrWhiteSpace(world) ||
            string.IsNullOrWhiteSpace(contentRoot))
        {
            return;
        }

        var worldPath = Path.GetFullPath(
            Path.IsPathRooted(world)
                ? world
                : Path.Combine(contentRoot, world));
        var assets = MauiProgram.GetService<AssetsService>();
        var contentFolder = assets.Folders.FirstOrDefault(folder =>
            string.Equals(
                Path.GetFullPath(folder.FullPath),
                Path.GetFullPath(contentRoot),
                StringComparison.OrdinalIgnoreCase));
        if (contentFolder is not null)
            await assets.EnsureFolderLoadedAsync(contentFolder.Id);

        var worldFile = assets.Assets.Values
            .OfType<SailorEditor.ViewModels.WorldFile>()
            .FirstOrDefault(asset => asset.Asset is not null &&
                string.Equals(
                    Path.GetFullPath(asset.Asset.FullName),
                    worldPath,
                    StringComparison.OrdinalIgnoreCase));
        if (worldFile is null ||
            !await MauiProgram.GetService<WorldService>().LoadWorldAsync(worldFile))
        {
            Console.Error.WriteLine(
                $"Unable to load command-line workspace world: {worldPath}");
        }
    }

    static string? ResolveCommandLineWorkspaceManifest(
        IReadOnlyList<string> arguments)
    {
        string? workspacePath = null;
        string? manifestPath = null;
        for (var index = 0; index < arguments.Count; ++index)
        {
            var argument = arguments[index];
            if (argument == "--workspace" && index + 1 < arguments.Count)
            {
                workspacePath = arguments[++index];
            }
            else if (argument == "--workspace-manifest" &&
                index + 1 < arguments.Count)
            {
                manifestPath = arguments[++index];
            }
        }

        if (!string.IsNullOrWhiteSpace(manifestPath))
            return Path.GetFullPath(manifestPath);
        if (string.IsNullOrWhiteSpace(workspacePath))
            return null;

        var fullWorkspacePath = Path.GetFullPath(workspacePath);
        if (File.Exists(fullWorkspacePath))
            return fullWorkspacePath;
        if (!Directory.Exists(fullWorkspacePath))
            return null;

        foreach (var filename in new[]
        {
            "SailorWorkspace.sailor",
            WorkspaceTemplateService.ManifestFileName
        })
        {
            var candidate = Path.Combine(fullWorkspacePath, filename);
            if (File.Exists(candidate))
                return candidate;
        }

        return Directory.EnumerateFiles(fullWorkspacePath, "*.sailor")
            .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault();
    }

    static string? ResolveCommandLineValue(
        IReadOnlyList<string> arguments,
        string option)
    {
        for (var index = 0; index + 1 < arguments.Count; ++index)
        {
            if (arguments[index] == option)
                return arguments[index + 1];
        }

        return null;
    }
}
