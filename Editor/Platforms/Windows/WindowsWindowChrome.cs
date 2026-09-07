using SailorEditor.Services;
using SailorEditor.Shell;

namespace SailorEditor.Platforms.Windows;

internal sealed class WindowsWindowChrome
{
    readonly EditorToolbarActions actions;
    readonly WorkspaceUiService workspace;
    readonly TitleBar titleBar;
    readonly Button simulationButton;

    WindowsWindowChrome(Window window)
    {
        actions = MauiProgram.GetService<EditorToolbarActions>();
        workspace = MauiProgram.GetService<WorkspaceUiService>();
        simulationButton = CreateButton("Simulate", "\uE7FC", () => actions.ToggleSimulationAsync());
        titleBar = new TitleBar
        {
            Title = "Sailor",
            HeightRequest = 40,
            BackgroundColor = Color.FromArgb("#202328"),
            ForegroundColor = Color.FromArgb("#DADCE0"),
            LeadingContent = CreateGroup(
                CreateButton("Save Scene", "\uE74E", actions.SaveAsync),
                CreateButton("Undo", "\uE7A7", actions.UndoAsync),
                CreateButton("Redo", "\uE7A6", actions.RedoAsync)),
            Content = CreateGroup(
                CreateButton("Play", "\uE768", () => actions.RunWorldAsync(false)),
                CreateButton("Debug Play", "\uE90F", () => actions.RunWorldAsync(true)),
                simulationButton),
            TrailingContent = CreateGroup(
                CreateButton("Trace Scene", "\uE722", () => actions.ExportPathTracedImageAsync(false)),
                CreateButton("Trace Selection", "\uE9D9", () => actions.ExportPathTracedImageAsync(true)),
                CreateButton("Save Layout", "\uE8A9", actions.SaveLayoutAsync),
                CreateButton("Reset Layout", "\uE777", actions.ResetLayoutAsync),
                CreateButton("Settings", "\uE713", actions.OpenSettingsAsync))
        };
        window.TitleBar = titleBar;
        actions.SimulationStateChanged += OnSimulationStateChanged;
        workspace.ProjectionChanged += OnWorkspaceChanged;
        window.Destroying += (_, _) =>
        {
            actions.SimulationStateChanged -= OnSimulationStateChanged;
            workspace.ProjectionChanged -= OnWorkspaceChanged;
        };
        UpdateTitle();
        UpdateSimulationButton(actions.IsSimulating);
    }

    public static void Configure(Window window) => _ = new WindowsWindowChrome(window);

    static HorizontalStackLayout CreateGroup(params View[] buttons)
    {
        var group = new HorizontalStackLayout
        {
            Spacing = 2,
            Margin = new Thickness(6, 0),
            HorizontalOptions = LayoutOptions.Center,
            VerticalOptions = LayoutOptions.Center
        };
        foreach (var button in buttons)
            group.Add(button);
        return group;
    }

    Button CreateButton(string label, string glyph, Func<Task> action)
    {
        var button = new Button
        {
            Text = glyph,
            FontFamily = "Segoe MDL2 Assets",
            FontSize = 16,
            TextColor = Color.FromArgb("#DADCE0"),
            BackgroundColor = Colors.Transparent,
            BorderWidth = 0,
            CornerRadius = 4,
            Padding = 0,
            WidthRequest = 32,
            HeightRequest = 32,
            MinimumWidthRequest = 32,
            MinimumHeightRequest = 32
        };
        ToolTipProperties.SetText(button, label);
        SemanticProperties.SetDescription(button, label);
        AutomationProperties.SetName(button, label);
        button.Clicked += async (_, _) =>
        {
            try
            {
                await action();
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine($"{label} failed: {exception}");
                MauiProgram.GetService<EditorShellHost>().SetStatus($"{label} failed: {exception.Message}");
            }
        };
        return button;
    }

    void OnWorkspaceChanged(object? sender, EventArgs args)
        => titleBar.Dispatcher.Dispatch(UpdateTitle);

    void UpdateTitle()
        => titleBar.Subtitle = workspace.Projection.ActiveWorkspaceName ?? "Engine Mode";

    void OnSimulationStateChanged(bool isSimulating)
        => titleBar.Dispatcher.Dispatch(() => UpdateSimulationButton(isSimulating));

    void UpdateSimulationButton(bool isSimulating)
    {
        var label = isSimulating ? "Stop Simulation" : "Simulate";
        simulationButton.Text = isSimulating ? "\uE71A" : "\uE7FC";
        simulationButton.TextColor = Color.FromArgb(isSimulating ? "#F06B6B" : "#DADCE0");
        ToolTipProperties.SetText(simulationButton, label);
        SemanticProperties.SetDescription(simulationButton, label);
        AutomationProperties.SetName(simulationButton, label);
    }
}
