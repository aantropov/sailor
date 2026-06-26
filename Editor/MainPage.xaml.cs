using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.Shell;
using SailorEditor.Styles;
using System.ComponentModel;
using System.Windows.Input;

namespace SailorEditor;

public partial class MainPage : ContentPage
{
    readonly EditorShellHost _shellHost;
    readonly WorkspaceUiService _workspaceUi;
    bool _workspaceUiInitialized;

    public ICommand ChangeThemeCommand => new Command<string>(ChangeTheme);

    public MainPage(EditorShellHost shellHost)
    {
        _shellHost = shellHost;
        _workspaceUi = MauiProgram.GetService<WorkspaceUiService>();
        InitializeComponent();
#if MACCATALYST
        Title = string.Empty;
        ToolbarHost.IsVisible = false;
        ToolbarHost.HeightRequest = 0;
        Microsoft.Maui.Controls.PlatformConfiguration.iOSSpecific.Page.SetUseSafeArea(this, false);
#endif
        ShellLayoutHost.Host = _shellHost;
        _shellHost.PropertyChanged += OnShellHostPropertyChanged;
        _workspaceUi.ProjectionChanged += OnWorkspaceProjectionChanged;
        BuildMenus();
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
            BuildMenus();
            UpdateStatusText();
        });
    }

    void BuildMenus()
    {
        MenuBarItems.Clear();

        var history = MauiProgram.GetService<ICommandHistoryService>();

        var file = new MenuBarItem { Text = "File" };
        file.Add(CreateWorkspaceMenuItem("New Workspace...", () => _workspaceUi.NewWorkspaceAsync()));
        file.Add(CreateWorkspaceMenuItem("Load Workspace...", () => _workspaceUi.OpenWorkspaceAsync()));
        file.Add(CreateWorkspaceMenuItem("Save Workspace", () => _workspaceUi.SaveWorkspaceAsync()));
        file.Add(BuildRecentWorkspacesMenu());
        file.Add(new MenuFlyoutSeparator());
        file.Add(new MenuFlyoutItem { Text = "Undo" , Command = new Command(async () => await history.UndoAsync(new CommandOrigin(CommandOriginKind.Menu, "Undo"))) });
        file.Add(new MenuFlyoutItem { Text = "Redo", Command = new Command(async () => await history.RedoAsync(new CommandOrigin(CommandOriginKind.Menu, "Redo"))) });
        file.Add(new MenuFlyoutSeparator());
        file.Add(new MenuFlyoutItem { Text = "Save Layout", Command = new Command(async () => await _shellHost.SaveLayoutAsync()) });
        file.Add(new MenuFlyoutItem { Text = "Reset Layout", Command = new Command(async () => await _shellHost.ResetLayoutAsync()) });
        file.Add(new MenuFlyoutSeparator());
        file.Add(new MenuFlyoutItem { Text = "Exit" });

        var window = new MenuBarItem { Text = "Window" };
        foreach (var panel in MauiProgram.GetService<Panels.PanelRegistry>().GetAllDescriptors().OrderBy(x => x.Title))
        {
            window.Add(new MenuFlyoutItem
            {
                Text = panel.Title,
                Command = new Command(async () => await _shellHost.OpenPanelAsync(panel.TypeId))
            });
        }

        var preferences = new MenuBarItem { Text = "Preferences" };
        preferences.Add(new MenuFlyoutItem { Text = "Light Theme", Command = ChangeThemeCommand, CommandParameter = "LightThemeStyle" });
        preferences.Add(new MenuFlyoutItem { Text = "Dark Theme", Command = ChangeThemeCommand, CommandParameter = "DarkThemeStyle" });

        MenuBarItems.Add(file);
        MenuBarItems.Add(window);
        MenuBarItems.Add(preferences);
    }

    MenuFlyoutSubItem BuildRecentWorkspacesMenu()
    {
        var recent = new MenuFlyoutSubItem { Text = "Load Recent Workspace" };
        if (_workspaceUi.Projection.RecentWorkspaces.Count == 0)
        {
            recent.Add(new MenuFlyoutItem { Text = "No recent workspaces", IsEnabled = false });
            return recent;
        }

        foreach (var workspace in _workspaceUi.Projection.RecentWorkspaces)
        {
            var manifestPath = workspace.ManifestPath;
            recent.Add(CreateWorkspaceMenuItem(
                $"{workspace.Name} - {workspace.DisplayPath}",
                () => _workspaceUi.OpenWorkspaceAsync(manifestPath)));
        }

        return recent;
    }

    MenuFlyoutItem CreateWorkspaceMenuItem(string text, Func<Task> action)
    {
        var item = new MenuFlyoutItem { Text = text };
        item.Clicked += async (_, _) => await RunWorkspaceMenuActionAsync(action);
        return item;
    }

    async Task RunWorkspaceMenuActionAsync(Func<Task> action)
    {
        try
        {
            await MainThread.InvokeOnMainThreadAsync(action);
        }
        catch (Exception ex)
        {
            await DisplayAlert("Workspace", ex.Message, "OK");
        }
    }

    void UpdateStatusText()
    {
        var projection = _workspaceUi.Projection;
        StatusTextLabel.Text = projection.HasActiveWorkspace
            ? $"Workspace: {projection.ActiveWorkspaceName} - {projection.ActiveWorkspacePath}"
            : _shellHost.StatusText;
    }

    void ChangeTheme(string theme)
    {
        var mergedDictionaries = Application.Current!.Resources.MergedDictionaries;
        mergedDictionaries.Clear();
        mergedDictionaries.Add(theme == "LightThemeStyle" ? new LightThemeStyle() : new DarkThemeStyle());
    }
}
