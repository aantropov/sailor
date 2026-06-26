#if MACCATALYST
using SailorEditor.Platforms.MacCatalyst;
#endif
using SailorEditor.Services;
using SailorEditor.Shell;
using SailorEditor.Workspace;
using System.ComponentModel;

namespace SailorEditor;

public partial class MainPage : ContentPage
{
    readonly EditorShellHost _shellHost;
    readonly WorkspaceUiService _workspaceUi;
    bool _workspaceUiInitialized;

    public MainPage(EditorShellHost shellHost)
    {
        _shellHost = shellHost;
        _workspaceUi = MauiProgram.GetService<WorkspaceUiService>();
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

        if (_shellHost.CurrentLayout is null)
            await _shellHost.InitializeAsync();
        ShellLayoutHost.Rebuild();
        UpdateStatusText();
    }

    void OnShellHostPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(EditorShellHost.StatusText))
            MainThread.BeginInvokeOnMainThread(UpdateStatusText);
        if (e.PropertyName == nameof(EditorShellHost.CurrentLayout))
            MainThread.BeginInvokeOnMainThread(() => ShellLayoutHost.Rebuild());
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
        StatusTextLabel.Text = projection.HasActiveWorkspace
            ? $"{_shellHost.StatusText} • Workspace: {projection.ActiveWorkspaceName} - {projection.ActiveWorkspacePath}"
            : _shellHost.StatusText;
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
        => projection.HasActiveWorkspace
            ? projection.ActiveWorkspaceName
            : "New Workspace";
}
