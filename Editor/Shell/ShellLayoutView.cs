using SailorEditor.Controls;
using SailorEditor.Layout;
using SailorEditor.Panels;
using SailorEditor.State;
using Microsoft.Maui.Controls.Shapes;
using MauiGrid = Microsoft.Maui.Controls.Grid;

namespace SailorEditor.Shell;

public sealed class ShellLayoutView : ContentView
{
    const string DragPanelIdKey = "ShellLayoutView.PanelId";
    const string DragSourceGroupIdKey = "ShellLayoutView.SourceGroupId";

    static readonly Color TabStripBackground = Color.FromArgb("#16171A");
    static readonly Color TabStripBorder = Color.FromArgb("#292A2E");
    static readonly Color PanelBackground = Color.FromArgb("#17181B");
    static readonly Color ActiveTabBackground = Color.FromArgb("#1C1D21");
    static readonly Color InactiveTabBackground = Color.FromArgb("#16171A");
    static readonly Color HoverTabBackground = Color.FromArgb("#232428");
    static readonly Color DropTargetTabBackground = Color.FromArgb("#263854");
    static readonly Color DropTargetTabStripBackground = Color.FromArgb("#1D2633");
    static readonly Color ActiveTabText = Color.FromArgb("#F1F1F1");
    static readonly Color InactiveTabText = Color.FromArgb("#90939A");
    static readonly Color CloseButtonText = Color.FromArgb("#6C6E75");
    static readonly Color SplitterColor = Color.FromArgb("#2A2C31");
    static readonly Color SplitterHoverColor = Color.FromArgb("#4D8DFF");

    public static readonly BindableProperty HostProperty = BindableProperty.Create(
        nameof(Host), typeof(EditorShellHost), typeof(ShellLayoutView), propertyChanged: OnHostChanged);

    public EditorShellHost? Host
    {
        get => (EditorShellHost?)GetValue(HostProperty);
        set => SetValue(HostProperty, value);
    }

    public bool IsResizing { get; private set; }

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
        if (IsResizing)
            return;

        Content = Host?.CurrentLayout is { } layout
            ? BuildNode(layout.Root.Content, [])
            : new Label { Text = "Loading shell layout…", Margin = 12 };
    }

    View BuildNode(LayoutNode node, IReadOnlyList<int> splitPath)
    {
        return node switch
        {
            SplitNode split => BuildSplit(split, splitPath),
            TabGroupNode tabs => BuildTabs(tabs),
            _ => new Label { Text = $"Unsupported layout node: {node.GetType().Name}" }
        };
    }

    View BuildSplit(SplitNode split, IReadOnlyList<int> splitPath)
    {
        var grid = new MauiGrid { RowSpacing = 0, ColumnSpacing = 0, BackgroundColor = Colors.Transparent };
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
                var childPath = splitPath.Concat([i]).ToArray();
                var child = BuildNode(split.Children[i], childPath);
                grid.Children.Add(child);
                grid.SetColumn(child, i * 2);

                if (i < split.Children.Count - 1)
                {
                    var splitter = CreateSplitter(split, grid, splitPath, i);
                    grid.Children.Add(splitter);
                    grid.SetColumn(splitter, i * 2 + 1);
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
                var childPath = splitPath.Concat([i]).ToArray();
                var child = BuildNode(split.Children[i], childPath);
                grid.Children.Add(child);
                grid.SetRow(child, i * 2);

                if (i < split.Children.Count - 1)
                {
                    var splitter = CreateSplitter(split, grid, splitPath, i);
                    grid.Children.Add(splitter);
                    grid.SetRow(splitter, i * 2 + 1);
                }
            }
        }

        return grid;
    }

    Border CreateSplitter(SplitNode split, MauiGrid grid, IReadOnlyList<int> splitPath, int leadingIndex)
    {
        var isHorizontal = split.Orientation == SplitOrientation.Horizontal;
        var splitter = new Border
        {
            BackgroundColor = SplitterColor,
            StrokeThickness = 0,
            Padding = 0,
            Margin = 0,
            WidthRequest = isHorizontal ? 4 : -1,
            HeightRequest = isHorizontal ? -1 : 4,
            MinimumWidthRequest = isHorizontal ? 4 : -1,
            MinimumHeightRequest = isHorizontal ? -1 : 4,
            HorizontalOptions = isHorizontal ? LayoutOptions.Center : LayoutOptions.Fill,
            VerticalOptions = isHorizontal ? LayoutOptions.Fill : LayoutOptions.Center,
            StrokeShape = new RoundRectangle { CornerRadius = new CornerRadius(0) },
            Content = new BoxView
            {
                BackgroundColor = Colors.Transparent,
                WidthRequest = isHorizontal ? 4 : -1,
                HeightRequest = isHorizontal ? -1 : 4,
                HorizontalOptions = isHorizontal ? LayoutOptions.Center : LayoutOptions.Fill,
                VerticalOptions = isHorizontal ? LayoutOptions.Fill : LayoutOptions.Center
            }
        };

        var pointer = new PointerGestureRecognizer();
        pointer.PointerEntered += (_, _) => splitter.BackgroundColor = SplitterHoverColor;
        pointer.PointerExited += (_, _) => splitter.BackgroundColor = SplitterColor;
        splitter.GestureRecognizers.Add(pointer);

        var pan = new PanGestureRecognizer();
        var startPrimary = 0d;
        var startSecondary = 0d;
        var totalAvailable = 0d;

        pan.PanUpdated += (_, e) =>
        {
            switch (e.StatusType)
            {
                case GestureStatus.Started:
                    IsResizing = true;
                    CaptureSplitSizes(split, grid, leadingIndex, out startPrimary, out startSecondary, out totalAvailable);
                    break;

                case GestureStatus.Running:
                    if (totalAvailable <= 0)
                        return;

                    var translation = isHorizontal ? e.TotalX : e.TotalY;
                    var primary = Math.Clamp(startPrimary + translation, 40d, totalAvailable - 40d);
                    var secondary = Math.Max(totalAvailable - primary, 40d);
                    ApplySplitSizes(split, grid, leadingIndex, primary, secondary);
                    break;

                case GestureStatus.Canceled:
                    IsResizing = false;
                    Rebuild();
                    break;

                case GestureStatus.Completed:
                    if (totalAvailable > 0 && Host is not null)
                    {
                        var translationDelta = isHorizontal ? e.TotalX : e.TotalY;
                        var ratioDelta = translationDelta / totalAvailable * (split.SizeRatios[leadingIndex] + split.SizeRatios[leadingIndex + 1]);
                        _ = MainThread.InvokeOnMainThreadAsync(async () =>
                        {
                            try
                            {
                                await Host.ResizeSplitAsync(splitPath, leadingIndex, ratioDelta);
                            }
                            finally
                            {
                                IsResizing = false;
                            }
                        });
                    }
                    else
                    {
                        IsResizing = false;
                        Rebuild();
                    }
                    break;
            }
        };

        splitter.GestureRecognizers.Add(pan);
        return splitter;
    }

    static void CaptureSplitSizes(SplitNode split, MauiGrid grid, int leadingIndex, out double primary, out double secondary, out double total)
    {
        if (split.Orientation == SplitOrientation.Horizontal)
        {
            var primaryColumn = leadingIndex * 2;
            var secondaryColumn = primaryColumn + 2;
            primary = ResolveDefinitionSize(grid.ColumnDefinitions[primaryColumn].Width, grid.Width * split.SizeRatios[leadingIndex]);
            secondary = ResolveDefinitionSize(grid.ColumnDefinitions[secondaryColumn].Width, grid.Width * split.SizeRatios[leadingIndex + 1]);
        }
        else
        {
            var primaryRow = leadingIndex * 2;
            var secondaryRow = primaryRow + 2;
            primary = ResolveDefinitionSize(grid.RowDefinitions[primaryRow].Height, grid.Height * split.SizeRatios[leadingIndex]);
            secondary = ResolveDefinitionSize(grid.RowDefinitions[secondaryRow].Height, grid.Height * split.SizeRatios[leadingIndex + 1]);
        }

        total = primary + secondary;
    }

    static void ApplySplitSizes(SplitNode split, MauiGrid grid, int leadingIndex, double primary, double secondary)
    {
        if (split.Orientation == SplitOrientation.Horizontal)
        {
            var primaryColumn = leadingIndex * 2;
            var secondaryColumn = primaryColumn + 2;
            grid.ColumnDefinitions[primaryColumn].Width = new GridLength(primary, GridUnitType.Absolute);
            grid.ColumnDefinitions[secondaryColumn].Width = new GridLength(secondary, GridUnitType.Absolute);
        }
        else
        {
            var primaryRow = leadingIndex * 2;
            var secondaryRow = primaryRow + 2;
            grid.RowDefinitions[primaryRow].Height = new GridLength(primary, GridUnitType.Absolute);
            grid.RowDefinitions[secondaryRow].Height = new GridLength(secondary, GridUnitType.Absolute);
        }
    }

    static double ResolveDefinitionSize(GridLength length, double fallback)
        => length.IsAbsolute && length.Value > 0 ? length.Value : Math.Max(fallback, 0);

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
            Content = active?.View ?? new Label { Text = "No panel", Margin = 12, TextColor = InactiveTabText },
            HorizontalOptions = LayoutOptions.Fill,
            VerticalOptions = LayoutOptions.Fill,
            BackgroundColor = Colors.Transparent
        };

        var tabBar = new HorizontalStackLayout
        {
            Spacing = 0,
            Padding = new Thickness(2, 2, 2, 0),
            BackgroundColor = TabStripBackground
        };
        AttachGroupDropTarget(tabBar, tabs.GroupId, () => tabBar.BackgroundColor, color => tabBar.BackgroundColor = color);

        var tabStateUpdaters = new List<Action<PanelId>>();
        foreach (var panel in panelInstances)
            tabBar.Children.Add(CreateTab(panel, active, contentHost, tabs.GroupId, tabStateUpdaters));

        var tabHeader = new Border
        {
            StrokeThickness = 1,
            Stroke = TabStripBorder,
            BackgroundColor = TabStripBackground,
            StrokeShape = new RoundRectangle { CornerRadius = new CornerRadius(0) },
            Content = tabBar
        };
        AttachGroupDropTarget(tabHeader, tabs.GroupId, () => tabHeader.BackgroundColor, color => tabHeader.BackgroundColor = color);

        var contentBorder = new Border
        {
            StrokeThickness = 1,
            Stroke = TabStripBorder,
            BackgroundColor = PanelBackground,
            StrokeShape = new RoundRectangle { CornerRadius = new CornerRadius(0) },
            Content = contentHost,
            Margin = new Thickness(0, -1, 0, 0)
        };

        return new MauiGrid
        {
            RowDefinitions =
            {
                new RowDefinition { Height = GridLength.Auto },
                new RowDefinition { Height = GridLength.Star }
            },
            Children =
            {
                tabHeader,
                contentBorder
            }
        }.AssignRows();
    }

    View CreateTab(PanelInstance panel, PanelInstance? active, ContentView contentHost, string? groupId, IList<Action<PanelId>> tabStateUpdaters)
    {
        var isActive = panel.PanelId == active?.PanelId;
        var textColor = isActive ? ActiveTabText : InactiveTabText;

        var title = new Label
        {
            Text = panel.Title,
            FontSize = 10,
            LineBreakMode = LineBreakMode.TailTruncation,
            VerticalTextAlignment = TextAlignment.Center,
            TextColor = textColor,
            Margin = new Thickness(0, 0, 4, 0),
            MaxLines = 1
        };

        var close = new Button
        {
            Text = "×",
            FontSize = 9,
            WidthRequest = 12,
            HeightRequest = 12,
            MinimumWidthRequest = 12,
            MinimumHeightRequest = 12,
            Padding = 0,
            Margin = 0,
            CornerRadius = 0,
            BackgroundColor = Colors.Transparent,
            TextColor = isActive ? InactiveTabText : CloseButtonText,
            BorderWidth = 0
        };
        close.Clicked += async (_, _) => await Host!.ClosePanelAsync(panel.PanelId);

        var tabContent = new MauiGrid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = GridLength.Star },
                new ColumnDefinition { Width = GridLength.Auto }
            },
            ColumnSpacing = 0,
            Padding = new Thickness(8, 2, 4, 2),
            MinimumWidthRequest = 64,
            HeightRequest = 20
        };
        tabContent.Children.Add(title);
        tabContent.Children.Add(close);
        tabContent.SetColumn(close, 1);

        var tab = new Border
        {
            BackgroundColor = isActive ? ActiveTabBackground : InactiveTabBackground,
            Stroke = TabStripBorder,
            StrokeThickness = 0.5,
            Padding = 0,
            Margin = new Thickness(0, 0, 1, 0),
            StrokeShape = new RoundRectangle { CornerRadius = new CornerRadius(0) },
            Content = tabContent
        };

        var currentIsActive = isActive;
        void SetActiveTab(PanelId activePanelId)
        {
            currentIsActive = panel.PanelId == activePanelId;
            tab.BackgroundColor = currentIsActive ? ActiveTabBackground : InactiveTabBackground;
            title.TextColor = currentIsActive ? ActiveTabText : InactiveTabText;
            close.TextColor = currentIsActive ? InactiveTabText : CloseButtonText;
        }

        tabStateUpdaters.Add(SetActiveTab);

        async void ActivateTab()
        {
            foreach (var update in tabStateUpdaters)
                update(panel.PanelId);

            contentHost.Content = panel.View;
            await Host!.FocusPanelLocalAsync(panel.PanelId);
        }

        var focusTap = new TapGestureRecognizer();
        focusTap.Tapped += (_, _) => ActivateTab();
        tab.GestureRecognizers.Add(focusTap);

        var contentFocusTap = new TapGestureRecognizer();
        contentFocusTap.Tapped += (_, _) => ActivateTab();
        tabContent.GestureRecognizers.Add(contentFocusTap);

        var drag = new DragGestureRecognizer();
        drag.DragStarting += (_, e) =>
        {
            e.Data.Properties[DragPanelIdKey] = panel.PanelId.Value;
            if (!string.IsNullOrWhiteSpace(groupId))
                e.Data.Properties[DragSourceGroupIdKey] = groupId!;
        };
        tab.GestureRecognizers.Add(drag);

        AttachGroupDropTarget(tab, groupId, () => tab.BackgroundColor, color => tab.BackgroundColor = color);

        if (!isActive)
        {
            var hover = new PointerGestureRecognizer();
            hover.PointerEntered += (_, _) =>
            {
                if (!currentIsActive)
                    tab.BackgroundColor = HoverTabBackground;
            };
            hover.PointerExited += (_, _) =>
            {
                if (!currentIsActive)
                    tab.BackgroundColor = InactiveTabBackground;
            };
            tab.GestureRecognizers.Add(hover);
        }

        return tab;
    }

    void AttachGroupDropTarget(View view, string? targetGroupId, Func<Color?> getBackground, Action<Color?> setBackground)
    {
        var drop = new DropGestureRecognizer();
        Color? restoreBackground = null;

        drop.DragOver += (_, e) =>
        {
			if (CanAcceptDrop(e.Data.Properties, targetGroupId))
			{
				restoreBackground ??= getBackground();
				setBackground(view is Border ? DropTargetTabBackground : DropTargetTabStripBackground);
				e.AcceptedOperation = DataPackageOperation.Copy;
			}
        };

        drop.DragLeave += (_, _) =>
        {
            if (restoreBackground is not null)
            {
                setBackground(restoreBackground);
                restoreBackground = null;
            }
        };

        drop.Drop += async (_, e) =>
        {
            try
            {
                if (TryGetDraggedPanelId(e.Data.Properties, out var panelId) && !string.IsNullOrWhiteSpace(targetGroupId))
                {
                    await Host!.MovePanelToGroupAsync(panelId, targetGroupId!);
                    e.Handled = true;
                }
            }
            finally
            {
                if (restoreBackground is not null)
                {
                    setBackground(restoreBackground);
                    restoreBackground = null;
                }
            }
        };

        view.GestureRecognizers.Add(drop);
    }

    static bool CanAcceptDrop(DataPackagePropertySet properties, string? targetGroupId)
    {
        return CanAcceptDropCore(properties.TryGetValue, targetGroupId);
    }

    static bool CanAcceptDrop(DataPackagePropertySetView properties, string? targetGroupId)
    {
        return CanAcceptDropCore(properties.TryGetValue, targetGroupId);
    }

    static bool CanAcceptDropCore(TryGetDragProperty tryGetProperty, string? targetGroupId)
    {
        if (string.IsNullOrWhiteSpace(targetGroupId) || !TryGetDraggedPanelId(tryGetProperty, out _))
            return false;

        if (tryGetProperty(DragSourceGroupIdKey, out var source) && source is string sourceGroupId)
            return !string.Equals(sourceGroupId, targetGroupId, StringComparison.Ordinal);

        return true;
    }

    static bool TryGetDraggedPanelId(DataPackagePropertySetView properties, out PanelId panelId)
    {
        return TryGetDraggedPanelId(properties.TryGetValue, out panelId);
    }

    static bool TryGetDraggedPanelId(TryGetDragProperty tryGetProperty, out PanelId panelId)
    {
        panelId = default;
        if (!tryGetProperty(DragPanelIdKey, out var raw) || raw is not string value || string.IsNullOrWhiteSpace(value))
            return false;

        panelId = new PanelId(value);
        return true;
    }

    delegate bool TryGetDragProperty(string key, out object value);
}

file static class ShellLayoutViewGridExtensions
{
    public static MauiGrid AssignRows(this MauiGrid grid)
    {
        if (grid.Children.Count > 0) grid.SetRow(grid.Children[0], 0);
        if (grid.Children.Count > 1) grid.SetRow(grid.Children[1], 1);
        return grid;
    }
}
