using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System.Collections.ObjectModel;
using System.Collections.Specialized;

namespace SailorEditor.Controls
{
    public struct TreeViewPopulateArgs
    {
        public TreeViewPopulateArgs() { }

        public string ItemImage { get; set; } = "blueprint.png";
        public string GroupImage { get; set; } = "folder.png";
        public bool ExpandGroupsByDefault { get; set; } = false;
    };

    public class TreeView : ScrollView
    {
        public class OnSelectItemEventArgs : EventArgs
        {
            public TreeViewNode Node { get; set; }
            public object Model { get; set; }
        }

        public class OnContextRequestedEventArgs : EventArgs
        {
            public TreeViewNode Node { get; set; }
            public object Model { get; set; }
        }

        public StackLayout StackLayout { get => _StackLayout; }

        private readonly StackLayout _StackLayout = new() { Orientation = StackOrientation.Vertical, BackgroundColor = Colors.Transparent };
        private readonly DropGestureRecognizer _RootDropGestureRecognizer = new();
        private readonly TapGestureRecognizer _RootContextGestureRecognizer = new() { Buttons = ButtonsMask.Secondary };

        //TODO: This initializes the list, but there is nothing listening to INotifyCollectionChanged so no nodes will get rendered
        private IList<TreeViewNode> _RootNodes = new ObservableCollection<TreeViewNode>();
        private TreeViewNode _SelectedItem;

        /// <summary>
        /// The item that is selected in the tree
        /// TODO: Make this two way - and maybe eventually a bindable property
        /// </summary>
        public TreeViewNode SelectedItem
        {
            get => _SelectedItem;

            set
            {
                if (_SelectedItem == value)
                {
                    return;
                }

                if (_SelectedItem != null)
                {
                    _SelectedItem.IsSelected = false;
                }

                _SelectedItem = value;

                var args = new OnSelectItemEventArgs() { Node = value, Model = value?.BindingContext };
                SelectedItemChanged?.Invoke(this, args);
            }
        }
        public IList<TreeViewNode> RootNodes
        {
            get => _RootNodes;
            set
            {
                _RootNodes = value;

                if (value is INotifyCollectionChanged notifyCollectionChanged)
                {
                    notifyCollectionChanged.CollectionChanged += (s, e) =>
                    {
                        RenderNodes(_RootNodes, _StackLayout, e, null);
                    };
                }

                RenderNodes(_RootNodes, _StackLayout, new NotifyCollectionChangedEventArgs(NotifyCollectionChangedAction.Reset), null);
            }
        }

        /// <summary>
        /// Occurs when the user selects a TreeViewItem
        /// </summary>
        public event EventHandler SelectedItemChanged;
        public event EventHandler<OnContextRequestedEventArgs> ContextRequested;
        public event EventHandler<TreeViewDropRequest> DropRequested;

        public TreeView()
        {
            BackgroundColor = Colors.Transparent;
            Content = _StackLayout;
            _RootDropGestureRecognizer.Drop += OnRootDrop;
            GestureRecognizers.Add(_RootDropGestureRecognizer);

            _RootContextGestureRecognizer.Tapped += OnRootContextRequested;
            GestureRecognizers.Add(_RootContextGestureRecognizer);
        }

        private void OnRootContextRequested(object sender, TappedEventArgs e)
        {
            ContextRequested?.Invoke(this, new OnContextRequestedEventArgs());
        }

        private void OnRootDrop(object sender, DropEventArgs e)
        {
            if (!e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var source))
            {
                return;
            }

            var request = new TreeViewDropRequest
            {
                Source = source,
                Target = null
            };

            RequestDrop(request);
            if (request.Handled)
            {
                e.Handled = true;
            }
        }

        private void RemoveSelectionRecursive(IEnumerable<TreeViewNode> nodes)
        {
            foreach (var treeViewItem in nodes)
            {
                if (treeViewItem != SelectedItem)
                {
                    treeViewItem.IsSelected = false;
                }

                RemoveSelectionRecursive(treeViewItem.ChildrenList);
            }
        }

        private static void AddItems(IEnumerable<TreeViewNode> childTreeViewItems, StackLayout parent, TreeViewNode parentTreeViewItem)
        {
            foreach (var childTreeNode in childTreeViewItems)
            {
                if (!parent.Children.Contains(childTreeNode))
                {
                    parent.Children.Add(childTreeNode);
                }

                childTreeNode.ParentTreeViewItem = parentTreeViewItem;
            }
        }

        /// <summary>
        /// TODO: A bit stinky but better than bubbling an event up...
        /// </summary>
        internal void ChildSelected(TreeViewNode child)
        {
            SelectedItem = child;
            child.IsSelected = true;
            child.SelectionBoxView.Color = child.SelectedBackgroundColor;
            child.SelectionBoxView.Opacity = child.SelectedBackgroundOpacity;
            RemoveSelectionRecursive(RootNodes);
        }

        internal void RequestDrop(TreeViewDropRequest request)
        {
            DropRequested?.Invoke(this, request);
        }

        internal void RequestContext(TreeViewNode node)
        {
            ContextRequested?.Invoke(this, new OnContextRequestedEventArgs()
            {
                Node = node,
                Model = node?.BindingContext
            });
        }

        internal static void RenderNodes(IEnumerable<TreeViewNode> childTreeViewItems, StackLayout parent, NotifyCollectionChangedEventArgs e, TreeViewNode parentTreeViewItem)
        {
            if (e.Action != NotifyCollectionChangedAction.Add)
            {
                parent.Children.Clear();
                AddItems(childTreeViewItems, parent, parentTreeViewItem);
            }
            else
            {
                AddItems(e.NewItems.Cast<TreeViewNode>(), parent, parentTreeViewItem);
            }
        }

        // Main code: 
        private static TreeViewNode CreateTreeViewNode(object bindingContext, Label label, bool isItem, TreeViewPopulateArgs args)
        {
            var node = new TreeViewNode
            {
                BindingContext = bindingContext,
                Content = new StackLayout
                {
                    Children =
                    {
                        new ResourceImage
                        {
                            Resource = isItem ? args.ItemImage : args.GroupImage,
                            HeightRequest= 16,
                            WidthRequest = 16
                        },
                        label
                    },
                    Orientation = StackOrientation.Horizontal
                }
            };

            //set DataTemplate for expand button content
            node.ExpandButtonTemplate = new DataTemplate(() => new ExpandButtonContent { BindingContext = node });

            return node;
        }

        private static void PopulateItem(IList<TreeViewNode> children, TreeViewItemBase itemModel, TreeViewPopulateArgs args)
        {
            var label = new Label
            {
                VerticalOptions = LayoutOptions.Center
            };
            label.SetBinding(Label.TextProperty, "Key");

            var xamlItemTreeViewNode = TreeView.CreateTreeViewNode(itemModel, label, true, args);
            children.Add(xamlItemTreeViewNode);
        }

        public static ObservableCollection<TreeViewNode> PopulateGroup(TreeViewItemGroupBase groupModel, TreeViewPopulateArgs args)
        {
            var res = PopulateGroup_Recursive(groupModel, args);

            ObservableCollection<TreeViewNode> items = new ();

            foreach (var item in groupModel.ChildrenItemsBase)
            {
                TreeView.PopulateItem(res, item, args);
            }

            return res;
        }

        static ObservableCollection<TreeViewNode> PopulateGroup_Recursive(TreeViewItemGroupBase groupModel, TreeViewPopulateArgs args)
        {
            var rootNodes = new ObservableCollection<TreeViewNode>();

            foreach (var itemGroup in groupModel.ChildrenGroupsBase)
            {
                var label = new Label
                {
                    VerticalOptions = LayoutOptions.Center
                };
                label.SetBinding(Label.TextProperty, "Key");

                var groupTreeViewNode = TreeView.CreateTreeViewNode(itemGroup, label, false, args);

                rootNodes.Add(groupTreeViewNode);

                groupTreeViewNode.ChildrenList = TreeView.PopulateGroup_Recursive(itemGroup, args);

                foreach (var item in itemGroup.ChildrenItemsBase)
                {
                    TreeView.PopulateItem(groupTreeViewNode.ChildrenList, item, args);
                }

                if (args.ExpandGroupsByDefault && groupTreeViewNode.ChildrenList.Count > 0)
                {
                    groupTreeViewNode.IsExpanded = true;
                }
            }

            return rootNodes;
        }
    }
}
