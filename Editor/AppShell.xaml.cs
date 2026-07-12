using SailorEditor.Commands;
#if MACCATALYST
using SailorEditor.Platforms.MacCatalyst;
#endif
using SailorEditor.Services;
using SailorEditor.Shell;
using SailorEditor.Styles;
using SailorEditor.Workspace;

namespace SailorEditor
{
    public partial class AppShell : Microsoft.Maui.Controls.Shell
    {
        readonly EditorShellHost _shellHost;
        readonly WorkspaceUiService _workspaceUi;

        public AppShell()
        {
            _shellHost = MauiProgram.GetService<EditorShellHost>();
            _workspaceUi = MauiProgram.GetService<WorkspaceUiService>();
            InitializeComponent();
            Title = "New Workspace";
            _workspaceUi.ProjectionChanged += OnWorkspaceProjectionChanged;
            UpdateWindowTitle();
            BuildMenus();
        }

        void OnWorkspaceProjectionChanged(object? sender, EventArgs e)
        {
            MainThread.BeginInvokeOnMainThread(() =>
            {
                UpdateWindowTitle();
#if !MACCATALYST
                BuildMenus();
#endif
            });
        }

        void BuildMenus()
        {
            MenuBarItems.Clear();

            var history = MauiProgram.GetService<ICommandHistoryService>();

            var file = new MenuBarItem { Text = "File" };
#if !MACCATALYST
            file.Add(CreateWorkspaceMenuItem("New Workspace...", () => _workspaceUi.NewWorkspaceAsync()));
            file.Add(CreateWorkspaceMenuItem("Open Workspace...", () => _workspaceUi.OpenWorkspaceAsync()));
            file.Add(CreateWorkspaceMenuItem("Save Workspace", () => _workspaceUi.SaveWorkspaceAsync()));
            file.Add(BuildRecentWorkspacesMenu());
            file.Add(new MenuFlyoutSeparator());
#endif
            file.Add(new MenuFlyoutItem { Text = "Undo", Command = new Command(async () => await history.UndoAsync(new CommandOrigin(CommandOriginKind.Menu, "Undo"))) });
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
            preferences.Add(new MenuFlyoutItem { Text = "Light Theme", Command = new Command(() => ChangeTheme("LightThemeStyle")) });
            preferences.Add(new MenuFlyoutItem { Text = "Dark Theme", Command = new Command(() => ChangeTheme("DarkThemeStyle")) });

            MenuBarItems.Add(file);
            MenuBarItems.Add(window);
            MenuBarItems.Add(preferences);
        }

        MenuFlyoutSubItem BuildRecentWorkspacesMenu()
        {
            var recent = new MenuFlyoutSubItem { Text = "Recent Workspaces" };
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

        static void ChangeTheme(string theme)
        {
            var mergedDictionaries = Application.Current!.Resources.MergedDictionaries;
            mergedDictionaries.Clear();
            mergedDictionaries.Add(theme == "LightThemeStyle" ? new LightThemeStyle() : new DarkThemeStyle());
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
}
