using SailorEditor.Controls;
using SailorEditor.Layout;
using SailorEditor.Panels;
using SailorEditor.State;

namespace SailorEditor.Shell;

public sealed class ShellLayoutView : ContentView
{
    public static readonly BindableProperty HostProperty = BindableProperty.Create(
        nameof(Host), typeof(EditorShellHost), typeof(ShellLayoutView), propertyChanged: OnHostChanged);

    public EditorShellHost? Host
    {
        get => (EditorShellHost?)GetValue(HostProperty);
        set => SetValue(HostProperty, value);
    }

    static void OnHostChanged(BindableObject bindable, object oldValue, object newValue)
    {
        var view = (ShellLayoutView)bindable;
        if (oldValue is EditorShellHost oldHost)
            oldHost.PropertyChanged -= view.OnHostPropertyChanged;
        if (newValue is EditorShellHost newHost)
            newHost.PropertyChanged += view.OnHostPropertyChanged;
        view.Rebuild();
    }

    void OnHostPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(EditorShellHost.State) or nameof(EditorShellHost.CurrentLayout))
            Rebuild();
    }

    public void Rebuild()
    {
        Content = Host?.CurrentLayout is { } layout
            ? BuildNode(layout.Root.Content)
            : new Label { Text = "Loading shell layout…", Margin = 12 };
    }

    View BuildNode(LayoutNode node)
    {
        return node switch
        {
            SplitNode split => BuildSplit(split),
            TabGroupNode tabs => BuildTabs(tabs),
            _ => new Label { Text = $"Unsupported layout node: {node.GetType().Name}" }
        };
    }

    View BuildSplit(SplitNode split)
    {
        var grid = new Grid { RowSpacing = 0, ColumnSpacing = 0, BackgroundColor = Colors.Transparent };
        var ratios = LayoutOperations.NormalizeRatios(split.SizeRatios, split.Children.Count);

        if (split.Orientation == SplitOrientation.Horizontal)
        {
            for (var i = 0; i < split.Children.Count; i++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(ratios[i], GridUnitType.Star) });
                if (i < split.Children.Count - 1)
                    grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            }

            for (var i = 0; i < split.Children.Count; i++)
            {
                grid.Children.Add(BuildNode(split.Children[i]));
                Grid.SetColumn(grid.Children[^1], i * 2);
                if (i < split.Children.Count - 1)
                {
                    var splitter = new VerticalGridSplitterControl();
                    grid.Children.Add(splitter);
                    Grid.SetColumn(splitter, i * 2 + 1);
                }
            }
        }
        else
        {
            for (var i = 0; i < split.Children.Count; i++)
            {
                grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(ratios[i], GridUnitType.Star) });
                if (i < split.Children.Count - 1)
                    grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            }

            for (var i = 0; i < split.Children.Count; i++)
            {
                grid.Children.Add(BuildNode(split.Children[i]));
                Grid.SetRow(grid.Children[^1], i * 2);
                if (i < split.Children.Count - 1)
                {
                    var splitter = new HorizontalGridSplitterControl();
                    grid.Children.Add(splitter);
                    Grid.SetRow(splitter, i * 2 + 1);
                }
            }
        }

        return grid;
    }

    View BuildTabs(TabGroupNode tabs)
    {
        var panelInstances = tabs.Panels
            .Select(panel => Host!.State.OpenPanels.FirstOrDefault(x => x.PanelId == panel.PanelId))
            .Where(x => x is not null && x.IsVisible)
            .Cast<PanelInstance>()
            .ToArray();

        var active = panelInstances.FirstOrDefault(x => x.PanelId == tabs.ActivePanelId) ?? panelInstances.FirstOrDefault();
        var contentHost = new ContentView
        {
            Content = active?.View ?? new Label { Text = "No panel" },
            HorizontalOptions = LayoutOptions.Fill,
            VerticalOptions = LayoutOptions.Fill
        };

        var tabBar = new HorizontalStackLayout { Spacing = 4, Padding = new Thickness(6, 4) };
        foreach (var panel in panelInstances)
        {
            var button = new Button
            {
                Text = panel.Title,
                Padding = new Thickness(10, 4),
                FontSize = 12,
                BackgroundColor = panel.PanelId == active?.PanelId ? Colors.DimGray : Colors.Transparent
            };
            button.Clicked += async (_, _) =>
            {
                await Host!.FocusPanelAsync(panel.PanelId);
                contentHost.Content = panel.View;
            };
            tabBar.Children.Add(button);

            var close = new Button { Text = "×", FontSize = 12, Padding = new Thickness(6, 2), BackgroundColor = Colors.Transparent };
            close.Clicked += async (_, _) => await Host!.ClosePanelAsync(panel.PanelId);
            tabBar.Children.Add(close);
        }

        return new Grid
        {
            RowDefinitions =
            {
                new RowDefinition { Height = GridLength.Auto },
                new RowDefinition { Height = GridLength.Star }
            },
            Children =
            {
                new Border
                {
                    StrokeThickness = 1,
                    Stroke = Colors.DimGray,
                    BackgroundColor = Color.FromArgb("#1E1E1E"),
                    Content = tabBar
                },
                new Border
                {
                    StrokeThickness = 1,
                    Stroke = Colors.DimGray,
                    BackgroundColor = Colors.Transparent,
                    Content = contentHost,
                    Margin = new Thickness(0, -1, 0, 0)
                }
            }
        }.AssignRows();
    }
}

file static class ShellLayoutViewGridExtensions
{
    public static Grid AssignRows(this Grid grid)
    {
        if (grid.Children.Count > 0) Grid.SetRow(grid.Children[0], 0);
        if (grid.Children.Count > 1) Grid.SetRow(grid.Children[1], 1);
        return grid;
    }
}
