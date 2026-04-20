using CommunityToolkit.Maui.Markup;
using SailorEditor.Commands;
using SailorEditor.Controls;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;
using SailorEngine;
using System.Collections.ObjectModel;

namespace SailorEditor.Views
{
    public partial class HierarchyView : ContentView
    {
        readonly WorldService service;
        readonly HierarchyProjectionService projectionService;
        readonly ObservableCollection<TreeViewNode> rootNodes = [];
        bool isApplyingSelectionFromService;

        public HierarchyView()
        {
            InitializeComponent();

            service = MauiProgram.GetService<WorldService>();
            projectionService = MauiProgram.GetService<HierarchyProjectionService>();

            HierarchyTree.RootNodes = rootNodes;
            service.OnUpdateWorldAction += PopulateHierarchyView;
            HierarchyTree.SelectedItemChanged += OnSelectTreeViewNode;
            HierarchyTree.ContextRequested += OnHierarchyContextRequested;
            HierarchyTree.DropRequested += OnHierarchyDropRequested;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectInstanceAction += SelectInstance;

            FlyoutBase.SetContextFlyout(HierarchyTree, CreateHierarchyContextFlyout(null));
            MainThread.BeginInvokeOnMainThread(() => PopulateHierarchyView(service.Current));
        }

        private void SelectInstance(InstanceId id)
        {
            if (id == null || id.IsEmpty())
            {
                HierarchyTree.SelectedItem = null;
                return;
            }

            var currentId = GetNodeInstanceId(HierarchyTree.SelectedItem);
            if (currentId == id)
            {
                return;
            }

            foreach (var el in HierarchyTree.RootNodes)
            {
                var res = FindInstanceNodeRecursive(el, id);
                if (res != null)
                {
                    isApplyingSelectionFromService = true;
                    try
                    {
                        ExpandAncestors(res);
                        res.Select();
                        HierarchyTree.SelectedItem = res;
                    }
                    finally
                    {
                        isApplyingSelectionFromService = false;
                    }
                    break;
                }
            }
        }

        static InstanceId GetNodeInstanceId(TreeViewNode? node)
        {
            return node?.BindingContext switch
            {
                TreeViewItem<Component> component => component.Model.InstanceId ?? InstanceId.NullInstanceId,
                TreeViewItem<GameObject> gameObject => gameObject.Model.InstanceId ?? InstanceId.NullInstanceId,
                TreeViewItemGroup<GameObject, Component> gameObject => gameObject.Model.InstanceId ?? InstanceId.NullInstanceId,
                _ => InstanceId.NullInstanceId
            };
        }

        static TreeViewNode? FindInstanceNodeRecursive(TreeViewNode parent, InstanceId id)
        {
            if (GetNodeInstanceId(parent) == id)
            {
                return parent;
            }

            foreach (var child in parent.ChildrenList)
            {
                var result = FindInstanceNodeRecursive(child, id);
                if (result != null)
                {
                    return result;
                }
            }

            return null;
        }

        public async void OnSelectTreeViewNode(object sender, EventArgs args)
        {
            if (isApplyingSelectionFromService)
            {
                return;
            }

            if (args is not TreeView.OnSelectItemEventArgs selectionChanged)
            {
                return;
            }

            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(HierarchyView)));

            switch (selectionChanged.Model)
            {
                case TreeViewItem<Component> component:
                    await dispatcher.DispatchAsync(new SelectObjectCommand(selectedObject: component.Model), context);
                    break;

                case TreeViewItemGroup<GameObject, Component> gameObject:
                    await dispatcher.DispatchAsync(new SelectObjectCommand(selectedObject: gameObject.Model), context);
                    break;
            }
        }

        private void OnHierarchyDropRequested(object sender, TreeViewDropRequest request)
        {
            var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            var contextProvider = MauiProgram.GetService<IActionContextProvider>();
            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(HierarchyView)));
            var target = request.Target as GameObject;

            if (!EditorDragDrop.TryCreateSceneDropCommand(request.Source, target, out var command) || command is null)
            {
                return;
            }

            request.Handled = dispatcher.DispatchAsync(command, context).GetAwaiter().GetResult().Succeeded;
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

            var existingRootNodesById = IndexNodes(rootNodes);
            var desiredRootNodes = projectionService.Roots
                .Select(node =>
                {
                    existingRootNodesById.TryGetValue(node.Id, out var existingNode);
                    return ReconcileHierarchyNode(node, lookup, existingNode);
                })
                .Where(node => node is not null)
                .Cast<TreeViewNode>()
                .ToList();

            ReplaceNodes(rootNodes, desiredRootNodes);

            var selectedId = projectionService.Roots.SelectMany(EnumerateProjectionNodes).FirstOrDefault(node => node.IsSelected)?.Id;
            if (!string.IsNullOrWhiteSpace(selectedId) && lookup.TryGetValue(selectedId, out var selectedGameObject) && selectedGameObject.InstanceId is not null)
            {
                SelectInstance(selectedGameObject.InstanceId);
            }
            else
            {
                HierarchyTree.SelectedItem = null;
            }
        }

        TreeViewNode? ReconcileHierarchyNode(HierarchyProjectionNode projection, IReadOnlyDictionary<string, GameObject> lookup, TreeViewNode? existingNode)
        {
            if (!lookup.TryGetValue(projection.Id, out var gameObject) || gameObject.InstanceId is null)
            {
                return null;
            }

            var node = existingNode ?? CreateHierarchyNode(gameObject, projection.Label);
            UpdateHierarchyNode(node, gameObject, projection.Label);

            var existingChildrenById = node.ChildrenList
                .Select(child => new { child, id = GetNodeInstanceId(child)?.Value })
                .Where(x => !string.IsNullOrWhiteSpace(x.id))
                .GroupBy(x => x.id!, StringComparer.Ordinal)
                .ToDictionary(group => group.Key, group => group.First().child, StringComparer.Ordinal);

            var desiredChildren = projection.Children
                .Select(childProjection =>
                {
                    existingChildrenById.TryGetValue(childProjection.Id, out var childNode);
                    return ReconcileHierarchyNode(childProjection, lookup, childNode);
                })
                .Where(child => child is not null)
                .Cast<TreeViewNode>()
                .ToList();

            ReplaceNodes(node.ChildrenList, desiredChildren);
            node.SetExpandedSilently(projection.IsExpanded);
            return node;
        }

        TreeViewNode CreateHierarchyNode(GameObject gameObject, string label)
        {
            var node = new TreeViewNode();
            if (gameObject.InstanceId is not null)
            {
                node.Expanded += (_, _) => projectionService.SetExpanded(gameObject.InstanceId, true);
                node.Collapsed += (_, _) => projectionService.SetExpanded(gameObject.InstanceId, false);
            }

            UpdateHierarchyNode(node, gameObject, label);
            return node;
        }

        void UpdateHierarchyNode(TreeViewNode node, GameObject gameObject, string label)
        {
            node.BindingContext = new TreeViewItemGroup<GameObject, Component>
            {
                Model = gameObject,
                Key = label
            };
            node.Content = CreateHierarchyNodeContent(label);
            node.ExpandButtonTemplate = new DataTemplate(() => new ExpandButtonContent { BindingContext = node });
            node.SetContextFlyout(CreateHierarchyContextFlyout(gameObject));
        }

        static View CreateHierarchyNodeContent(string label)
        {
            return new StackLayout
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
                        Text = label,
                        VerticalOptions = LayoutOptions.Center
                    }
                },
                Orientation = StackOrientation.Horizontal
            };
        }

        static Dictionary<string, TreeViewNode> IndexNodes(IEnumerable<TreeViewNode> nodes)
        {
            var result = new Dictionary<string, TreeViewNode>(StringComparer.Ordinal);
            foreach (var node in nodes)
            {
                var instanceId = GetNodeInstanceId(node)?.Value;
                if (!string.IsNullOrWhiteSpace(instanceId))
                {
                    result[instanceId] = node;
                }

                foreach (var pair in IndexNodes(node.ChildrenList))
                {
                    result[pair.Key] = pair.Value;
                }
            }

            return result;
        }

        static void ReplaceNodes(IList<TreeViewNode> target, IReadOnlyList<TreeViewNode> desired)
        {
            if (target is ObservableCollection<TreeViewNode> observable)
            {
                observable.Clear();
                foreach (var node in desired)
                {
                    observable.Add(node);
                }
                return;
            }

            target.Clear();
            foreach (var node in desired)
            {
                target.Add(node);
            }
        }

        static IEnumerable<HierarchyProjectionNode> EnumerateProjectionNodes(HierarchyProjectionNode node)
        {
            yield return node;
            foreach (var child in node.Children.SelectMany(EnumerateProjectionNodes))
            {
                yield return child;
            }
        }

        static void ExpandAncestors(TreeViewNode node)
        {
            var parent = node.ParentTreeViewItem;
            while (parent is not null)
            {
                parent.SetExpandedSilently(true);
                parent = parent.ParentTreeViewItem;
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
