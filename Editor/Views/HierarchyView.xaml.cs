using CommunityToolkit.Maui.Markup;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Controls;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;
using SailorEngine;

namespace SailorEditor.Views
{
    public partial class HierarchyView : ContentView
    {
        WorldService service;
        readonly HierarchyProjectionService projectionService;

        public HierarchyView()
        {
            InitializeComponent();

            this.service = MauiProgram.GetService<WorldService>();
            projectionService = MauiProgram.GetService<HierarchyProjectionService>();

            service.OnUpdateWorldAction += PopulateHierarchyView;
            HierarchyTree.SelectedItemChanged += OnSelectTreeViewNode;
            HierarchyTree.ContextRequested += OnHierarchyContextRequested;
            HierarchyTree.DropRequested += OnHierarchyDropRequested;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectInstanceAction += SelectInstance;
        }

        private void SelectInstance(InstanceId id)
        {
            if (id == null || id.IsEmpty())
            {
                return;
            }

            var currentId = HierarchyTree.SelectedItem?.BindingContext switch
            {
                TreeViewItem<Component> component => component.Model.InstanceId,
                TreeViewItemGroup<GameObject, Component> gameObject => gameObject.Model.InstanceId,
                _ => InstanceId.NullInstanceId
            };

            if (currentId == id)
            {
                return;
            }

            foreach (var el in HierarchyTree.RootNodes)
            {
                var res = el.FindItemRecursive(id);
                if (res != null)
                {
                    res.Select();
                    HierarchyTree.SelectedItem = res;
                    break;
                }
            }
        }

        public static void OnSelectTreeViewNode(object sender, EventArgs args)
        {
            var selectionChanged = args as TreeView.OnSelectItemEventArgs;
            if (selectionChanged != null)
            {
                var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
                var contextProvider = MauiProgram.GetService<IActionContextProvider>();

                switch (selectionChanged.Model)
                {
                    case TreeViewItem<Component> component:
                        dispatcher.DispatchAsync(new SelectObjectCommand(selectedObject: component.Model), contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(HierarchyView)))).GetAwaiter().GetResult();
                        break;

                    case TreeViewItemGroup<GameObject, Component> gameObject:
                        dispatcher.DispatchAsync(new SelectObjectCommand(selectedObject: gameObject.Model), contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(HierarchyView)))).GetAwaiter().GetResult();
                        break;
                }
            }
        }

        private void OnHierarchyDropRequested(object sender, Utility.TreeViewDropRequest request)
        {
            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(HierarchyView)));

            if (request.Target is null)
            {
                request.Handled = request.Source switch
                {
                    GameObject source => service.ReparentToRoot(source, keepWorldTransform: true),
                    PrefabFile prefab => dispatcher.DispatchAsync(new InstantiatePrefabAssetCommand(prefab), context).GetAwaiter().GetResult().Succeeded,
                    _ => false
                };
                return;
            }

            if (request.Target is not GameObject target)
            {
                return;
            }

            request.Handled = request.Source switch
            {
                GameObject source => service.Reparent(source, target, keepWorldTransform: true),
                PrefabFile prefab => dispatcher.DispatchAsync(new InstantiatePrefabAssetCommand(prefab, target), context).GetAwaiter().GetResult().Succeeded,
                _ => false
            };
        }

        private void OnHierarchyContextRequested(object sender, TreeView.OnContextRequestedEventArgs args)
        {
            var contextMenu = MauiProgram.GetService<EditorContextMenuService>();
            var model = args.Model switch
            {
                TreeViewItemGroup<GameObject, Component> group => group.Model,
                TreeViewItem<GameObject> item => item.Model,
                _ => null
            };

            if (model is GameObject gameObject)
            {
                contextMenu.Show(
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
                return;
            }

            contextMenu.Show(new EditorContextMenuItem
            {
                Text = "Create new GameObject",
                Command = new Command(() => service.CreateGameObject())
            });
        }

        private void PopulateHierarchyView(World world)
        {
            projectionService.Refresh();

            var lookup = world.Prefabs
                .SelectMany(prefab => prefab.GameObjects)
                .Where(gameObject => gameObject.InstanceId is not null && !gameObject.InstanceId.IsEmpty())
                .ToDictionary(gameObject => gameObject.InstanceId!.Value, StringComparer.Ordinal);

            var rootNodes = projectionService.Roots
                .Select(node => CreateHierarchyNode(node, lookup))
                .Where(node => node is not null)
                .Cast<TreeViewNode>()
                .ToList();

            HierarchyTree.RootNodes = rootNodes;
            FlyoutBase.SetContextFlyout(HierarchyTree, CreateHierarchyContextFlyout(null));
            foreach (var node in rootNodes)
            {
                AttachHierarchyContextFlyouts(node);
            }

            var selectedId = projectionService.Roots.SelectMany(EnumerateProjectionNodes).FirstOrDefault(node => node.IsSelected)?.Id;
            if (!string.IsNullOrWhiteSpace(selectedId) && lookup.TryGetValue(selectedId, out var selectedGameObject) && selectedGameObject.InstanceId is not null)
            {
                SelectInstance(selectedGameObject.InstanceId);
            }
        }

        TreeViewNode? CreateHierarchyNode(HierarchyProjectionNode projection, IReadOnlyDictionary<string, GameObject> lookup)
        {
            if (!lookup.TryGetValue(projection.Id, out var gameObject))
            {
                return null;
            }

            var node = new TreeViewNode
            {
                BindingContext = new TreeViewItemGroup<GameObject, Component>
                {
                    Model = gameObject,
                    Key = projection.Label
                },
                Content = new StackLayout
                {
                    Children =
                    {
                        new ResourceImage
                        {
                            Resource = "blue_document.png",
                            HeightRequest = 16,
                            WidthRequest = 16
                        },
                        new Label
                        {
                            Text = projection.Label,
                            VerticalOptions = LayoutOptions.Center
                        }
                    },
                    Orientation = StackOrientation.Horizontal
                }
            };

            node.ExpandButtonTemplate = new DataTemplate(() => new ExpandButtonContent { BindingContext = node });

            var children = projection.Children
                .Select(child => CreateHierarchyNode(child, lookup))
                .Where(child => child is not null)
                .Cast<TreeViewNode>()
                .ToList();

            node.ChildrenList = children;
            node.IsExpanded = projection.IsExpanded;
            if (gameObject.InstanceId is not null)
            {
                node.Expanded += (_, _) => projectionService.SetExpanded(gameObject.InstanceId, true);
                node.Collapsed += (_, _) => projectionService.SetExpanded(gameObject.InstanceId, false);
            }

            return node;
        }

        static IEnumerable<HierarchyProjectionNode> EnumerateProjectionNodes(HierarchyProjectionNode node)
        {
            yield return node;
            foreach (var child in node.Children.SelectMany(EnumerateProjectionNodes))
            {
                yield return child;
            }
        }

        void AttachHierarchyContextFlyouts(TreeViewNode node)
        {
            var model = node.BindingContext switch
            {
                TreeViewItemGroup<GameObject, Component> group => group.Model,
                TreeViewItem<GameObject> item => item.Model,
                _ => null
            };

            if (model is GameObject gameObject)
            {
                node.SetContextFlyout(CreateHierarchyContextFlyout(gameObject));
            }

            foreach (var child in node.ChildrenList)
            {
                AttachHierarchyContextFlyouts(child);
            }
        }

        MenuFlyout CreateHierarchyContextFlyout(GameObject gameObject)
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
