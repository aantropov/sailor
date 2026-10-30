using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Panels;
using SailorEditor.Services;
using SailorEditor.Shell;
using SailorEditor.Utility;
using SailorEditor.Workflow;
using SailorEngine;
using System.Collections.ObjectModel;
using GameObject = SailorEditor.ViewModels.GameObject;
using PrefabFile = SailorEditor.ViewModels.PrefabFile;
using World = SailorEditor.ViewModels.World;

namespace SailorEditor.Views
{
    public partial class HierarchyView : ContentView
    {
        const double IndentWidth = 14.0;

        readonly WorldService service;
        readonly HierarchyProjectionService projectionService;
        readonly InspectorPendingEditCoordinator inspectorPendingEditCoordinator;
        readonly GameObjectClipboardService gameObjectClipboardService;
        readonly ObservableCollection<HierarchyListRow> visibleRows = [];
        readonly Dictionary<string, GameObject> gameObjectsById = new(StringComparer.Ordinal);
        readonly SelectionService selectionService;
        readonly HashSet<Border> rowBorders = [];
        string? selectionAnchorId;
        long hierarchyWorkspaceEpoch;

        public HierarchyView()
        {
            InitializeComponent();

            service = MauiProgram.GetService<WorldService>();
            projectionService = MauiProgram.GetService<HierarchyProjectionService>();
            inspectorPendingEditCoordinator =
                MauiProgram.GetService<InspectorPendingEditCoordinator>();
            gameObjectClipboardService =
                MauiProgram.GetService<GameObjectClipboardService>();

            HierarchyList.ItemsSource = visibleRows;

            service.OnUpdateWorldAction += PopulateHierarchyView;

            selectionService = MauiProgram.GetService<SelectionService>();
            selectionService.OnSelectInstanceAction += SelectInstance;
            selectionService.PropertyChanged += (_, args) =>
            {
                if (args.PropertyName == nameof(SelectionService.SelectedInstanceIds))
                {
                    ApplyProjection(service.Current);
                }
            };

            var rootDropGesture = new DropGestureRecognizer();
            rootDropGesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Copy;
            rootDropGesture.Drop += OnHierarchyRootDrop;
            HierarchyDropSurface.GestureRecognizers.Add(rootDropGesture);

            var listRootDropGesture = new DropGestureRecognizer();
            listRootDropGesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Copy;
            listRootDropGesture.Drop += OnHierarchyRootDrop;
            HierarchyList.GestureRecognizers.Add(listRootDropGesture);
            HierarchyRootDropOverlay.GestureRecognizers.Add(CreateRootDropGesture());
            HierarchyDropSurface.SizeChanged += (_, _) => UpdateRootDropOverlay();

            FlyoutBase.SetContextFlyout(HierarchyList, CreateHierarchyContextFlyout(null));
            FlyoutBase.SetContextFlyout(HierarchyDropSurface, CreateHierarchyContextFlyout(null));
            FlyoutBase.SetContextFlyout(HierarchyRootDropOverlay, CreateHierarchyContextFlyout(null));
            HierarchyList.ItemTemplate = CreateItemTemplate();

            MainThread.BeginInvokeOnMainThread(() => PopulateHierarchyView(service.Current));
        }

        void SelectInstance(InstanceId id)
        {
            if (id is not null && !id.IsEmpty())
            {
                selectionAnchorId ??= id.Value;
                projectionService.EnsureVisible(id);
            }
            ApplyProjection(service.Current);
        }

        async Task SelectHierarchyRowAsync(
            HierarchyListRow row,
            bool preserveSelected = false,
            bool useModifiers = true)
        {
            if (!gameObjectsById.ContainsKey(row.InstanceId))
            {
                return;
            }

            var modifiers = useModifiers ? EditorInputModifiers.Current : default;
            var selection = ListSelectionPolicy.Select(
                visibleRows.Select(item => item.InstanceId).ToArray(),
                selectionService.SelectedItems.OfType<GameObject>()
                    .Select(item => item.InstanceId.Value),
                row.InstanceId,
                selectionAnchorId,
                modifiers.Shift,
                modifiers.Additive,
                preserveSelected);
            selectionAnchorId = selection.AnchorId;
            await selectionService.SelectObjectsAsync(selection.SelectedIds
                .Where(gameObjectsById.ContainsKey)
                .Select(id => (ObservableObject)gameObjectsById[id]));
            ApplyProjection(service.Current);
        }

        IReadOnlyList<GameObject> GetSelectedGameObjects(GameObject? contextItem = null, bool rootsOnly = false)
        {
            var selected = selectionService.SelectedItems.OfType<GameObject>().ToArray();
            if (contextItem is not null && !selected.Any(item => item.InstanceId.Equals(contextItem.InstanceId)))
            {
                selected = [contextItem];
            }
            return rootsOnly
                ? EditorDragDrop.GetSelectionRoots(selected).OfType<GameObject>().ToArray()
                : selected;
        }

        async void OnHierarchyRootDrop(object? sender, DropEventArgs e)
            => await ExecuteHierarchyActionAsync(
                () => HandleDrop(e, null),
                "Drop hierarchy item");

        async Task HandleDrop(DropEventArgs e, GameObject? target)
        {
            if (e.Handled)
            {
                return;
            }

            if (!e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var source))
            {
                return;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(HierarchyView)));

            if (!EditorDragDrop.TryCreateSceneDropCommand(source, target, out var command) || command is null)
            {
                if (source is GameObject or EditorSelectionDragPayload)
                {
                    e.Handled = true;
                }
                return;
            }

            e.Handled = true;
            var result = await dispatcher.DispatchAsync(command, context);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(result.Message ?? "Drop hierarchy items failed.");
            }
            var createdIds = EditorCommandBatch.ResultValues<InstanceId>(result).ToArray();
            if (createdIds.Length > 0)
            {
                selectionService.SelectInstances(createdIds);
            }
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

            if (hierarchyWorkspaceEpoch != projectionService.WorkspaceEpoch)
            {
                hierarchyWorkspaceEpoch = projectionService.WorkspaceEpoch;
                selectionAnchorId = null;
            }
            var desiredRows = projectionService.VisibleRows;
            gameObjectsById.Clear();
            foreach (var gameObject in world.Prefabs
                .SelectMany(prefab => prefab.GameObjects)
                .Where(gameObject => gameObject.InstanceId is not null && !gameObject.InstanceId.IsEmpty()))
            {
                gameObjectsById[gameObject.InstanceId!.Value] = gameObject;
            }
            if (selectionAnchorId is not null && !gameObjectsById.ContainsKey(selectionAnchorId))
            {
                selectionAnchorId = selectionService.SelectedInstanceIds.LastOrDefault()?.Value;
            }

            HierarchyRowReconciler.Reconcile(visibleRows, desiredRows);
            UpdateRootDropOverlay();
            foreach (var border in rowBorders)
            {
                UpdateRowHighlight(border);
            }
        }

        void UpdateRowHighlight(Border border)
        {
            if (border.BindingContext is not HierarchyListRow row)
            {
                return;
            }
            var selected = selectionService.SelectedInstanceIds.Any(id =>
                string.Equals(id.Value, row.InstanceId, StringComparison.Ordinal));
            border.BackgroundColor = selected ? Color.FromArgb("#334C6FFF") : Colors.Transparent;
            border.Stroke = Color.FromArgb("#779DDB");
            border.StrokeThickness = selected && string.Equals(
                selectionService.SelectedInstanceId?.Value, row.InstanceId, StringComparison.Ordinal) ? 1 : 0;
        }

        DropGestureRecognizer CreateRootDropGesture()
        {
            var gesture = new DropGestureRecognizer();
            gesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Copy;
            gesture.Drop += OnHierarchyRootDrop;
            return gesture;
        }

        void UpdateRootDropOverlay()
        {
            const double rowHeight = 26.0;
            const double listMargin = 4.0;
            var overlayTop = listMargin + visibleRows.Count * rowHeight;
            var overlayHeight = Math.Max(0.0, HierarchyDropSurface.Height - overlayTop - listMargin);
            HierarchyRootDropOverlay.TranslationY = overlayTop;
            HierarchyRootDropOverlay.HeightRequest = overlayHeight;
            HierarchyRootDropOverlay.IsVisible = overlayHeight > 1.0;
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

                var prefabLinkLabel = new Label
                {
                    WidthRequest = 14,
                    HorizontalTextAlignment = TextAlignment.Center,
                    VerticalTextAlignment = TextAlignment.Center,
                    FontSize = 9,
                    TextColor = Color.FromArgb("#74B9FF"),
                    Margin = new Thickness(0, 0, 3, 0)
                };
                prefabLinkLabel.SetBinding(
                    Label.TextProperty,
                    nameof(HierarchyListRow.PrefabLinkGlyph));
                prefabLinkLabel.SetBinding(
                    IsVisibleProperty,
                    nameof(HierarchyListRow.IsPrefabLinked));

                var rowLayout = new HorizontalStackLayout
                {
                    Spacing = 0,
                    Padding = new Thickness(4, 2),
                    HeightRequest = 24,
                    VerticalOptions = LayoutOptions.Center,
                    HorizontalOptions = LayoutOptions.Fill
                };
                rowLayout.Children.Add(expandLabel);
                rowLayout.Children.Add(prefabLinkLabel);
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
                        if (!GetSelectedGameObjects().Any(item => item.InstanceId.Equals(gameObject.InstanceId)))
                        {
                            selectionAnchorId = row.InstanceId;
                            selectionService.SelectInstances([gameObject.InstanceId]);
                        }
                        e.Data.Properties[EditorDragDrop.DragItemKey] =
                            EditorDragDrop.CreateSelectionPayload(GetSelectedGameObjects(), gameObject);
                    }
                };
                border.GestureRecognizers.Add(dragGesture);

                var dropGesture = new DropGestureRecognizer();
                dropGesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Copy;
                dropGesture.Drop += (_, e) =>
                {
                    if (border.BindingContext is not HierarchyListRow row)
                    {
                        return;
                    }

                    gameObjectsById.TryGetValue(row.InstanceId, out var target);
                    _ = ExecuteHierarchyActionAsync(
                        () => HandleDrop(e, target),
                        "Drop hierarchy item");
                };
                border.GestureRecognizers.Add(dropGesture);

                var selectGesture = new TapGestureRecognizer
                {
                    NumberOfTapsRequired = 1
                };
                selectGesture.Tapped += (_, _) =>
                {
                    if (border.BindingContext is HierarchyListRow row)
                    {
                        _ = ExecuteHierarchyActionAsync(
                            () => SelectHierarchyRowAsync(
                                row),
                            "Select GameObject");
                    }
                };
                border.GestureRecognizers.Add(selectGesture);

                var focusGesture = new TapGestureRecognizer
                {
                    NumberOfTapsRequired = 2
                };
                focusGesture.Tapped += (_, _) =>
                {
                    if (border.BindingContext is HierarchyListRow row)
                    {
                        _ = ExecuteHierarchyActionAsync(
                            async () =>
                            {
                                await SelectHierarchyRowAsync(
                                    row,
                                    preserveSelected: true,
                                    useModifiers: false);
                                await FocusGameObjectAsync(row);
                            },
                            "Focus GameObject");
                    }
                };
                border.GestureRecognizers.Add(focusGesture);

                var contextTap = new TapGestureRecognizer { Buttons = ButtonsMask.Secondary };
                contextTap.Tapped += (_, _) =>
                {
                    if (border.BindingContext is HierarchyListRow row)
                    {
                        _ = ExecuteHierarchyActionAsync(
                        () => SelectHierarchyRowAsync(row, preserveSelected: true, useModifiers: false),
                        "Select GameObjects");
                    }
                };
                border.GestureRecognizers.Add(contextTap);
                rowBorders.Add(border);

                border.BindingContextChanged += (_, _) =>
                {
                    if (border.BindingContext is HierarchyListRow row)
                    {
                        rowLayout.Margin = new Thickness(row.Depth * IndentWidth, 0, 0, 0);
                        UpdateRowHighlight(border);

                        if (gameObjectsById.TryGetValue(row.InstanceId, out var gameObject))
                        {
                            FlyoutBase.SetContextFlyout(
                                border,
                                CreateHierarchyContextFlyout(gameObject, row));
                            return;
                        }
                    }

                    FlyoutBase.SetContextFlyout(border, null);
                };

                return border;
            });
        }

        MenuFlyout CreateHierarchyContextFlyout(
            GameObject? gameObject,
            HierarchyListRow? row = null)
        {
            var contextMenu = MauiProgram.GetService<EditorContextMenuService>();
            if (gameObject == null)
            {
                return contextMenu.CreateFlyout(
                    new EditorContextMenuItem
                    {
                        Text = "New GameObject",
                        Command = CreateContextMenuCommand(
                            () => service.CreateGameObjectAsync(),
                            "Create GameObject")
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Copy",
                        KeyboardAccelerators = CreateKeyboardAccelerators("C"),
                        Command = CreateContextMenuCommand(
                            CopySelectedGameObjectAsync,
                            "Copy GameObject")
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Paste",
                        KeyboardAccelerators = CreateKeyboardAccelerators("V"),
                        Command = CreateContextMenuCommand(
                            PasteGameObjectAsync,
                            "Paste GameObject")
                    });
            }

            var items = new List<EditorContextMenuItem>
            {
                new EditorContextMenuItem
                {
                    Text = "Copy",
                    KeyboardAccelerators = CreateKeyboardAccelerators("C"),
                    Command = CreateContextMenuCommand(() => CopyGameObjectsAsync(gameObject), "Copy GameObjects")
                },
                new EditorContextMenuItem
                {
                    Text = "Paste",
                    KeyboardAccelerators = CreateKeyboardAccelerators("V"),
                    Command = CreateContextMenuCommand(PasteGameObjectAsync, "Paste GameObjects")
                },
                new EditorContextMenuItem
                {
                    Text = "New GameObject",
                    Command = CreateContextMenuCommand(
                        () => service.CreateGameObjectAsync(gameObject),
                        "Create GameObject")
                },
                new EditorContextMenuItem
                {
                    Text = "Rename",
                    Command = CreateContextMenuCommand(
                        () => RenameGameObjectsAsync(gameObject),
                        "Rename GameObject")
                },
                new EditorContextMenuItem
                {
                    Text = "Duplicate",
                    Command = CreateContextMenuCommand(
                        () => DuplicateGameObjectAsync(gameObject),
                        "Duplicate GameObject")
                },
                new EditorContextMenuItem
                {
                    Text = "Remove",
                    IsDestructive = true,
                    Command = CreateContextMenuCommand(
                        () => RemoveGameObjectsAsync(gameObject),
                        "Remove GameObject")
                }
            };
            if (row is
                {
                    IsPrefabLinked: true,
                    PrefabFileId: not null
                } &&
                !string.IsNullOrWhiteSpace(row.PrefabFileId))
            {
                items.Insert(
                    2,
                    new EditorContextMenuItem
                    {
                        Text = "Select Prefab",
                        Command = CreateContextMenuCommand(
                            () => SelectPrefabAsync(row.PrefabFileId),
                            "Select Prefab")
                    });

                if (row is
                    {
                        PrefabRootInstanceId: not null
                    } &&
                    !string.IsNullOrWhiteSpace(row.PrefabRootInstanceId))
                {
                    items.Insert(
                        3,
                        new EditorContextMenuItem
                        {
                            Text = "Break Prefab Link",
                            Command = CreateContextMenuCommand(
                                () => BreakPrefabLinkAsync(
                                    new InstanceId(
                                        row.PrefabRootInstanceId),
                                    row.PrefabFileId),
                                "Break Prefab Link")
                        });
                }
            }

            return contextMenu.CreateFlyout([.. items]);
        }

        static IReadOnlyList<KeyboardAccelerator> CreateKeyboardAccelerators(
            string key)
            =>
            [
                new KeyboardAccelerator
                {
                    Key = key,
                    Modifiers = KeyboardAcceleratorModifiers.Ctrl
                },
                new KeyboardAccelerator
                {
                    Key = key,
                    Modifiers = KeyboardAcceleratorModifiers.Cmd
                }
            ];

        async Task CommitPendingInspectorChangesAsync()
        {
            if (!await inspectorPendingEditCoordinator
                    .CommitPendingChangesAsync())
            {
                throw new InvalidOperationException(
                    "Pending Inspector changes could not be committed.");
            }
        }

        Task CopySelectedGameObjectAsync() => CopyGameObjectsAsync(null);

        async Task CopyGameObjectsAsync(GameObject? contextItem)
        {
            var selected = GetSelectedGameObjects(contextItem, rootsOnly: true);
            if (selected.Count == 0)
            {
                return;
            }
            await CommitPendingInspectorChangesAsync();
            await gameObjectClipboardService.CopyAsync(selected);
        }

        async Task PasteGameObjectAsync()
        {
            await CommitPendingInspectorChangesAsync();
            await gameObjectClipboardService.PasteAsync();
        }

        async Task DuplicateGameObjectAsync(GameObject gameObject)
        {
            var selected = GetSelectedGameObjects(gameObject, rootsOnly: true);
            await CommitPendingInspectorChangesAsync();
            await gameObjectClipboardService.DuplicateAsync(selected);
        }

        async Task RemoveGameObjectsAsync(GameObject contextItem)
        {
            var selected = GetSelectedGameObjects(contextItem, rootsOnly: true);
            await CommitPendingInspectorChangesAsync();
            await DispatchGroupAsync("Remove GameObjects", selected.Select(item =>
                (IEditorCommand)new DestroyGameObjectCommand(item)));
        }

        async Task DispatchGroupAsync(string description, IEnumerable<IEditorCommand> commands)
        {
            var context = MauiProgram.GetService<IActionContextProvider>().GetCurrentContext(
                new CommandOrigin(CommandOriginKind.Menu, nameof(HierarchyView)));
            var result = await MauiProgram.GetService<ICommandDispatcher>().DispatchAsync(
                EditorCommandBatch.Create(description, commands), context);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(result.Message ?? description + " failed.");
            }
        }

        static Command CreateContextMenuCommand(
            Func<Task> action,
            string operation)
        {
            return new Command(
                () => RunContextMenuAction(
                    action,
                    operation));
        }

        static void RunContextMenuAction(
            Func<Task> action,
            string operation)
        {
            _ = ExecuteContextMenuActionAsync(
                action,
                operation);
        }

        static async Task ExecuteContextMenuActionAsync(
            Func<Task> action,
            string operation)
        {
            try
            {
                await action();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"{operation} failed: {ex}");
                try
                {
                    MauiProgram.GetService<EditorShellHost>()
                        .SetStatus($"{operation} failed: {ex.Message}");
                }
                catch (Exception statusException)
                {
                    Console.Error.WriteLine(
                        $"Failed to publish hierarchy action error status: {statusException}");
                }
            }
        }

        async Task FocusGameObjectAsync(HierarchyListRow row)
        {
            try
            {
                if (!await inspectorPendingEditCoordinator
                        .CommitPendingChangesAsync())
                {
                    throw new InvalidOperationException(
                        "Pending Inspector changes could not be committed.");
                }

                if (!gameObjectsById.TryGetValue(
                        row.InstanceId,
                        out var gameObject) ||
                    gameObject.InstanceId is null ||
                    gameObject.InstanceId.IsEmpty())
                {
                    return;
                }

                var contextProvider =
                    MauiProgram.GetService<IActionContextProvider>();
                var context = contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(HierarchyView)));
                var result = await MauiProgram
                    .GetService<ICommandDispatcher>()
                    .DispatchAsync(
                        new FocusEditorCameraCommand(
                            gameObject.InstanceId),
                        context);
                if (!result.Succeeded)
                {
                    throw new InvalidOperationException(
                        result.Message ??
                        "Focus editor camera failed");
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"Focus editor camera failed: {ex}");
            }
        }

        async Task BreakPrefabLinkAsync(
            InstanceId instanceId,
            string prefabFileId)
        {
            var contextProvider =
                MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(
                new CommandOrigin(
                    CommandOriginKind.Menu,
                    nameof(HierarchyView)));
            var result = await MauiProgram
                .GetService<ICommandDispatcher>()
                .DispatchAsync(
                    new BreakPrefabLinkCommand(
                        instanceId,
                        new FileId(prefabFileId)),
                    context);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(
                    result.Message ?? "Break prefab link failed");
            }
        }

        async Task SelectPrefabAsync(string prefabFileId)
        {
            var fileId = new FileId(prefabFileId);
            if (!MauiProgram.GetService<AssetsService>()
                    .Assets.TryGetValue(fileId, out var asset) ||
                asset is not PrefabFile prefab)
            {
                throw new InvalidOperationException(
                    $"Prefab asset '{prefabFileId}' is not available.");
            }

            await MauiProgram.GetService<EditorShellHost>()
                .OpenPanelAsync(KnownPanelTypes.Content);

            var contextProvider =
                MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(
                new CommandOrigin(
                    CommandOriginKind.Menu,
                    nameof(HierarchyView)));
            var result = await MauiProgram
                .GetService<ICommandDispatcher>()
                .DispatchAsync(
                    new OpenAssetCommand(prefab),
                    context);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(
                    result.Message ?? "Select prefab failed");
            }
        }

        static async Task ExecuteHierarchyActionAsync(
            Func<Task> action,
            string operation)
        {
            try
            {
                await action();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"{operation} failed: {ex}");
            }
        }

        async Task RenameGameObjectsAsync(GameObject contextItem)
        {
            var selected = GetSelectedGameObjects(contextItem);
            await CommitPendingInspectorChangesAsync();
            var commands = new List<IEditorCommand>();
            foreach (var gameObject in selected)
            {
                var newName = await Application.Current.MainPage.DisplayPromptAsync(
                    "Rename GameObject", $"New name for {gameObject.Name}", "OK", "Cancel", initialValue: gameObject.Name);
                if (string.IsNullOrWhiteSpace(newName))
                {
                    return;
                }
                var before = EditorYaml.SerializeGameObject(gameObject);
                var clone = (GameObject)gameObject.Clone();
                clone.Name = newName.Trim();
                commands.Add(new UpdateGameObjectCommand(gameObject, before,
                    EditorYaml.SerializeGameObject(clone), $"Rename {gameObject.Name}"));
            }
            await DispatchGroupAsync("Rename GameObjects", commands);
        }

    }
}
