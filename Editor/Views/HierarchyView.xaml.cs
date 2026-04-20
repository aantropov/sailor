using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.Workflow;
using SailorEngine;
using System.Collections.ObjectModel;

namespace SailorEditor.Views
{
    public partial class HierarchyView : ContentView
    {
        const double IndentWidth = 14.0;

        readonly WorldService service;
        readonly HierarchyProjectionService projectionService;
        readonly ObservableCollection<HierarchyListRow> visibleRows = [];
        readonly Dictionary<string, GameObject> gameObjectsById = new(StringComparer.Ordinal);
        bool suppressSelectionChanged;

        public HierarchyView()
        {
            InitializeComponent();

            service = MauiProgram.GetService<WorldService>();
            projectionService = MauiProgram.GetService<HierarchyProjectionService>();

            HierarchyList.ItemsSource = visibleRows;
            HierarchyList.SelectionChanged += OnHierarchySelectionChanged;

            service.OnUpdateWorldAction += PopulateHierarchyView;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectInstanceAction += SelectInstance;

            var rootDropGesture = new DropGestureRecognizer();
            rootDropGesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Move;
            rootDropGesture.Drop += OnHierarchyRootDrop;
            HierarchyRootDropZone.GestureRecognizers.Add(rootDropGesture);

            FlyoutBase.SetContextFlyout(HierarchyList, CreateHierarchyContextFlyout(null));
            HierarchyList.ItemTemplate = CreateItemTemplate();

            MainThread.BeginInvokeOnMainThread(() => PopulateHierarchyView(service.Current));
        }

        void SelectInstance(InstanceId id)
        {
            suppressSelectionChanged = true;
            try
            {
                if (id == null || id.IsEmpty())
                {
                    HierarchyList.SelectedItem = null;
                    return;
                }

                projectionService.EnsureVisible(id);
                ApplyProjection(service.Current);
                HierarchyList.SelectedItem = visibleRows.FirstOrDefault(row => string.Equals(row.InstanceId, id.Value, StringComparison.Ordinal));
            }
            finally
            {
                suppressSelectionChanged = false;
            }
        }

        async void OnHierarchySelectionChanged(object? sender, SelectionChangedEventArgs args)
        {
            if (suppressSelectionChanged)
            {
                return;
            }

            if (args.CurrentSelection.FirstOrDefault() is not HierarchyListRow row)
            {
                return;
            }

            if (!gameObjectsById.TryGetValue(row.InstanceId, out var gameObject))
            {
                return;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(HierarchyView)));
            await dispatcher.DispatchAsync(new SelectObjectCommand(selectedObject: gameObject), context);
        }

        void OnHierarchyRootDrop(object? sender, DropEventArgs e)
        {
            HandleDrop(e, null);
        }

        void HandleDrop(DropEventArgs e, GameObject? target)
        {
            if (!e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var source))
            {
                return;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(HierarchyView)));

            if (!EditorDragDrop.TryCreateSceneDropCommand(source, target, out var command) || command is null)
            {
                return;
            }

            e.Handled = dispatcher.DispatchAsync(command, context).GetAwaiter().GetResult().Succeeded;
        }

        void ToggleRowExpansion(HierarchyListRow row)
        {
            if (!row.HasChildren || !gameObjectsById.TryGetValue(row.InstanceId, out var gameObject) || gameObject.InstanceId is null)
            {
                return;
            }

            projectionService.SetExpanded(gameObject.InstanceId, !row.IsExpanded);
            ApplyProjection(service.Current);
        }

        void PopulateHierarchyView(World world)
        {
            projectionService.Refresh();
            ApplyProjection(world);
        }

        void ApplyProjection(World world)
        {
            using var perfScope = EditorPerf.Scope("HierarchyView.ApplyProjection");

            gameObjectsById.Clear();
            foreach (var gameObject in world.Prefabs
                .SelectMany(prefab => prefab.GameObjects)
                .Where(gameObject => gameObject.InstanceId is not null && !gameObject.InstanceId.IsEmpty()))
            {
                gameObjectsById[gameObject.InstanceId!.Value] = gameObject;
            }

            ReplaceRows(visibleRows, projectionService.VisibleRows);

            suppressSelectionChanged = true;
            try
            {
                HierarchyList.SelectedItem = visibleRows.FirstOrDefault(row => row.IsSelected);
            }
            finally
            {
                suppressSelectionChanged = false;
            }
        }

        static void ReplaceRows(ObservableCollection<HierarchyListRow> target, IReadOnlyList<HierarchyListRow> desired)
        {
            target.Clear();
            foreach (var row in desired)
            {
                target.Add(row);
            }
        }

        DataTemplate CreateItemTemplate()
        {
            return new DataTemplate(() =>
            {
                var expandLabel = new Label
                {
                    WidthRequest = 12,
                    HorizontalTextAlignment = TextAlignment.Center,
                    VerticalTextAlignment = TextAlignment.Center,
                    FontSize = 10,
                    Margin = new Thickness(0, 0, 4, 0)
                };
                expandLabel.SetBinding(Label.TextProperty, nameof(HierarchyListRow.ExpandGlyph));
                expandLabel.SetBinding(IsVisibleProperty, nameof(HierarchyListRow.HasChildren));
                var expandTap = new TapGestureRecognizer();
                expandTap.Tapped += (_, _) =>
                {
                    if (expandLabel.BindingContext is HierarchyListRow row)
                    {
                        ToggleRowExpansion(row);
                    }
                };
                expandLabel.GestureRecognizers.Add(expandTap);

                var nameLabel = new Label
                {
                    VerticalTextAlignment = TextAlignment.Center,
                    LineBreakMode = LineBreakMode.TailTruncation
                };
                nameLabel.SetBinding(Label.TextProperty, nameof(HierarchyListRow.Label));

                var rowLayout = new HorizontalStackLayout
                {
                    Spacing = 0,
                    Padding = new Thickness(4, 2),
                    HeightRequest = 24,
                    VerticalOptions = LayoutOptions.Center,
                    HorizontalOptions = LayoutOptions.Fill
                };
                rowLayout.Children.Add(expandLabel);
                rowLayout.Children.Add(nameLabel);

                var border = new Border
                {
                    StrokeThickness = 0,
                    Padding = 0,
                    Margin = new Thickness(2, 1),
                    Content = rowLayout
                };

                var dragGesture = new DragGestureRecognizer();
                dragGesture.DragStarting += (_, e) =>
                {
                    if (border.BindingContext is HierarchyListRow row && gameObjectsById.TryGetValue(row.InstanceId, out var gameObject))
                    {
                        e.Data.Properties[EditorDragDrop.DragItemKey] = gameObject;
                    }
                };
                border.GestureRecognizers.Add(dragGesture);

                var dropGesture = new DropGestureRecognizer();
                dropGesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Move;
                dropGesture.Drop += (_, e) =>
                {
                    if (border.BindingContext is not HierarchyListRow row)
                    {
                        return;
                    }

                    gameObjectsById.TryGetValue(row.InstanceId, out var target);
                    HandleDrop(e, target);
                };
                border.GestureRecognizers.Add(dropGesture);

                border.BindingContextChanged += (_, _) =>
                {
                    if (border.BindingContext is HierarchyListRow row)
                    {
                        rowLayout.Margin = new Thickness(row.Depth * IndentWidth, 0, 0, 0);
                        border.BackgroundColor = row.IsSelected ? Color.FromArgb("#334C6FFF") : Colors.Transparent;

                        if (gameObjectsById.TryGetValue(row.InstanceId, out var gameObject))
                        {
                            FlyoutBase.SetContextFlyout(border, CreateHierarchyContextFlyout(gameObject));
                            return;
                        }
                    }

                    FlyoutBase.SetContextFlyout(border, null);
                };

                return border;
            });
        }

        MenuFlyout CreateHierarchyContextFlyout(GameObject? gameObject)
        {
            var contextMenu = MauiProgram.GetService<EditorContextMenuService>();
            if (gameObject == null)
            {
                return contextMenu.CreateFlyout(new EditorContextMenuItem
                {
                    Text = "Create new GameObject",
                    Command = new Command(() => service.CreateGameObject())
                });
            }

            return contextMenu.CreateFlyout(
                new EditorContextMenuItem
                {
                    Text = "Create Child GameObject",
                    Command = new Command(() => service.CreateGameObject(gameObject))
                },
                new EditorContextMenuItem
                {
                    Text = "Rename",
                    Command = new Command(async () => await RenameGameObject(gameObject))
                },
                new EditorContextMenuItem
                {
                    Text = "Remove",
                    IsDestructive = true,
                    Command = new Command(() => service.RemoveGameObject(gameObject))
                });
        }

        async Task RenameGameObject(GameObject gameObject)
        {
            var newName = await Application.Current.MainPage.DisplayPromptAsync(
                "Rename GameObject",
                "New name",
                "OK",
                "Cancel",
                initialValue: gameObject.Name);

            if (string.IsNullOrWhiteSpace(newName))
            {
                return;
            }

            service.RenameGameObject(gameObject, newName.Trim());
        }
    }
}
