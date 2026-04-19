using SailorEditor.Commands;
using SailorEditor.Shell;
using SailorEditor.Styles;
using System.ComponentModel;
using System.Windows.Input;

namespace SailorEditor;

public partial class MainPage : ContentPage
{
    readonly EditorShellHost _shellHost;

    public ICommand ChangeThemeCommand => new Command<string>(ChangeTheme);

    public MainPage(EditorShellHost shellHost)
    {
        _shellHost = shellHost;
        InitializeComponent();
        ShellLayoutHost.Host = _shellHost;
        _shellHost.PropertyChanged += OnShellHostPropertyChanged;
        BuildMenus();
    }

    protected override async void OnAppearing()
    {
        base.OnAppearing();
        if (_shellHost.CurrentLayout is null)
            await _shellHost.InitializeAsync();
        ShellLayoutHost.Rebuild();
        StatusTextLabel.Text = _shellHost.StatusText;
    }

    void OnShellHostPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(EditorShellHost.StatusText))
            MainThread.BeginInvokeOnMainThread(() => StatusTextLabel.Text = _shellHost.StatusText);
        if (e.PropertyName == nameof(EditorShellHost.CurrentLayout))
            MainThread.BeginInvokeOnMainThread(() => ShellLayoutHost.Rebuild());
    }

    void BuildMenus()
    {
        MenuBarItems.Clear();

        var history = MauiProgram.GetService<ICommandHistoryService>();

        var file = new MenuBarItem { Text = "File" };
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

    void ChangeTheme(string theme)
    {
        var mergedDictionaries = Application.Current!.Resources.MergedDictionaries;
        mergedDictionaries.Clear();
        mergedDictionaries.Add(theme == "LightThemeStyle" ? new LightThemeStyle() : new DarkThemeStyle());
    }
}
