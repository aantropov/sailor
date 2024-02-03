//6
using SailorEditor.ViewModels;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Security.Cryptography.X509Certificates;

namespace SailorEditor.Controls
{
    public class TreeView : ScrollView
    {
        public class OnSelectItemEventArgs : EventArgs
        {
            public TreeViewNode Node { get; set; }
            public object Model { get; set; }
        }

        private readonly StackLayout _StackLayout = new() { Orientation = StackOrientation.Vertical };

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

        public TreeView()
        {
            Content = _StackLayout;
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

        internal static void RenderNodes(IEnumerable<TreeViewNode> childTreeViewItems, StackLayout parent, NotifyCollectionChangedEventArgs e, TreeViewNode parentTreeViewItem)
        {
            if (e.Action != NotifyCollectionChangedAction.Add)
            {
                //TODO: Reintate this...
                //parent.Children.Clear();
                AddItems(childTreeViewItems, parent, parentTreeViewItem);
            }
            else
            {
                AddItems(e.NewItems.Cast<TreeViewNode>(), parent, parentTreeViewItem);
            }
        }

        // Main code: 
        private static TreeViewNode CreateTreeViewNode(object bindingContext, Label label, bool isItem)
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
                            Resource = isItem? "blueprint.png" : "folder.png",
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

        private static void PopulateItem(IList<TreeViewNode> children, TreeViewItemBase itemModel)
        {
            var label = new Label
            {
                VerticalOptions = LayoutOptions.Center,
                TextColor = Colors.Black
            };
            label.SetBinding(Label.TextProperty, "Key");

            var xamlItemTreeViewNode = TreeView.CreateTreeViewNode(itemModel, label, true);
            children.Add(xamlItemTreeViewNode);
        }

        /*        private static void ProcessXamlItems(TreeViewNode node, XamlItemGroup xamlItemGroup)
                {
                    var children = new ObservableCollection<TreeViewNode>();
                    foreach (var xamlItem in xamlItemGroup.XamlItems.OrderBy(xi => xi.Key))
                    {
                        CreateXamlItem(children, xamlItem);
                    }
                    node.ChildrenList = children;
                }
        */
        public static ObservableCollection<TreeViewNode> PopulateGroup(TreeViewItemGroupBase groupModel)
        {
            var rootNodes = new ObservableCollection<TreeViewNode>();

            foreach (var itemGroup in groupModel.ChildrenGroupsBase.OrderBy(xig => xig.Key))
            {
                var label = new Label
                {
                    VerticalOptions = LayoutOptions.Center,
                    TextColor = Colors.Black
                };
                label.SetBinding(Label.TextProperty, "Key");

                var groupTreeViewNode = TreeView.CreateTreeViewNode(itemGroup, label, false);

                rootNodes.Add(groupTreeViewNode);

                groupTreeViewNode.ChildrenList = TreeView.PopulateGroup(itemGroup);

                foreach (var item in itemGroup.ChildrenItemsBase)
                {
                    TreeView.PopulateItem(groupTreeViewNode.ChildrenList, item);
                }
            }

            return rootNodes;
        }
    }
}
