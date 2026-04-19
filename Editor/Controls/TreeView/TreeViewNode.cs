using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System.Collections.ObjectModel;
using System.Collections.Specialized;

namespace SailorEditor.Controls
{
    public class TreeViewNode : StackLayout
    {
        private DataTemplate _ExpandButtonTemplate = null;

        private TreeViewNode _ParentTreeViewItem;

        private DateTime _ExpandButtonClickedTime;

        private readonly BoxView _SpacerBoxView = new() { Color = Colors.Transparent };
        private readonly BoxView _EmptyBox = new() { BackgroundColor = Colors.Blue, Opacity = .5 };

        private readonly ContentView _ExpandButtonContent = new();

        private readonly Grid _MainGrid = new()
        {
            VerticalOptions = LayoutOptions.Start,
            HorizontalOptions = LayoutOptions.Fill,
            RowSpacing = 2
        };

        private readonly StackLayout _ContentStackLayout = new() { Orientation = StackOrientation.Horizontal };
        private readonly ContentView _ContentView = new() { HorizontalOptions = LayoutOptions.Fill, BackgroundColor = Colors.Transparent };
        private readonly StackLayout _ChildrenStackLayout = new()
        {
            Orientation = StackOrientation.Vertical,
            Spacing = 0,
            IsVisible = false
        };

        private IList<TreeViewNode> _Children = new ObservableCollection<TreeViewNode>();
        private readonly TapGestureRecognizer _TapGestureRecognizer = new();
        private readonly TapGestureRecognizer _ContextGestureRecognizer = new() { Buttons = ButtonsMask.Secondary };
        private readonly TapGestureRecognizer _ExpandButtonGestureRecognizer = new();
        private readonly TapGestureRecognizer _DoubleClickGestureRecognizer = new();
        private readonly DragGestureRecognizer _DragGestureRecognizer = new();
        private readonly DropGestureRecognizer _DropGestureRecognizer = new();

        internal readonly BoxView SelectionBoxView = new() { Color = Colors.Blue, Opacity = .5, IsVisible = false };

        private TreeView ParentTreeView => FindParentTreeView();
        private double IndentWidth => Depth * SpacerWidth;
        private int SpacerWidth { get; } = 30;
        private int Depth => ParentTreeViewItem?.Depth + 1 ?? 0;

        private bool _ShowExpandButtonIfEmpty = false;
        private Color _SelectedBackgroundColor = Colors.Blue;
        private double _SelectedBackgroundOpacity = .3;

        public event EventHandler Expanded;
        public event EventHandler Collapsed;

        /// <summary>
        /// Occurs when the user double clicks on the node
        /// </summary>
        public event EventHandler DoubleClicked;

        protected override void OnParentSet()
        {
            base.OnParentSet();
            Render();
        }

        public bool IsSelected
        {
            get => SelectionBoxView.IsVisible;
            set => SelectionBoxView.IsVisible = value;
        }
        public bool IsExpanded
        {
            get => _ChildrenStackLayout.IsVisible;
            set
            {
                if (_ChildrenStackLayout.IsVisible == value)
                {
                    return;
                }

                _ChildrenStackLayout.IsVisible = value;

                Render();
                if (value)
                {
                    Expanded?.Invoke(this, EventArgs.Empty);
                }
                else
                {
                    Collapsed?.Invoke(this, EventArgs.Empty);
                }
            }
        }

        /// <summary>
        /// set to true to show the expand button in case we need to poulate the child nodes on demand
        /// </summary>
        public bool ShowExpandButtonIfEmpty
        {
            get { return _ShowExpandButtonIfEmpty; }
            set { _ShowExpandButtonIfEmpty = value; }
        }

        /// <summary>
        /// set BackgroundColor when node is tapped/selected
        /// </summary>
        public Color SelectedBackgroundColor
        {
            get { return _SelectedBackgroundColor; }
            set { _SelectedBackgroundColor = value; }
        }

        /// <summary>
        /// SelectedBackgroundOpacity when node is tapped/selected
        /// </summary>
        public Double SelectedBackgroundOpacity
        {
            get { return _SelectedBackgroundOpacity; }
            set { _SelectedBackgroundOpacity = value; }
        }

        /// <summary>
        /// customize expand icon based on isExpanded property and or data 
        /// </summary>
        public DataTemplate ExpandButtonTemplate
        {
            get { return _ExpandButtonTemplate; }
            set { _ExpandButtonTemplate = value; }
        }

        public View Content
        {
            get => _ContentView.Content;
            set => _ContentView.Content = value;
        }

        public void SetContextFlyout(MenuFlyout flyout)
        {
            FlyoutBase.SetContextFlyout(this, flyout);
            FlyoutBase.SetContextFlyout(_ContentStackLayout, flyout);
            FlyoutBase.SetContextFlyout(_ContentView, flyout);
            if (_ContentView.Content is VisualElement content)
            {
                FlyoutBase.SetContextFlyout(content, flyout);
            }
        }

        public IList<TreeViewNode> ChildrenList
        {
            get => _Children;
            set
            {
                if (_Children is INotifyCollectionChanged notifyCollectionChanged)
                {
                    notifyCollectionChanged.CollectionChanged -= ItemsSource_CollectionChanged;
                }

                _Children = value;

                if (_Children is INotifyCollectionChanged notifyCollectionChanged2)
                {
                    notifyCollectionChanged2.CollectionChanged += ItemsSource_CollectionChanged;
                }

                TreeView.RenderNodes(_Children, _ChildrenStackLayout, new NotifyCollectionChangedEventArgs(NotifyCollectionChangedAction.Reset), this);

                Render();
            }
        }

        /// <summary>
        /// TODO: Remove this. We should be able to get the ParentTreeViewNode by traversing up through the Visual Tree by 'Parent', but this not working for some reason.
        /// </summary>
        public TreeViewNode ParentTreeViewItem
        {
            get => _ParentTreeViewItem;
            set
            {
                _ParentTreeViewItem = value;
                Render();
            }
        }

        /// <summary>
        /// Constructs a new TreeViewItem
        /// </summary>
        public TreeViewNode()
        {
            BackgroundColor = Colors.Transparent;

            var itemsSource = (ObservableCollection<TreeViewNode>)_Children;
            itemsSource.CollectionChanged += ItemsSource_CollectionChanged;

            _TapGestureRecognizer.Tapped += TapGestureRecognizer_Tapped;
            GestureRecognizers.Add(_TapGestureRecognizer);

            _ContextGestureRecognizer.Tapped += ContextGestureRecognizer_Tapped;
            _ContentView.GestureRecognizers.Add(_ContextGestureRecognizer);

            _MainGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            _MainGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            _MainGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            _MainGrid.Children.Add(SelectionBoxView);

            _ContentStackLayout.Children.Add(_SpacerBoxView);
            _ContentStackLayout.Children.Add(_ExpandButtonContent);
            _ContentStackLayout.Children.Add(_ContentView);

            SetExpandButtonContent(_ExpandButtonTemplate);

            _ExpandButtonGestureRecognizer.Tapped += ExpandButton_Tapped;
            _ExpandButtonContent.GestureRecognizers.Add(_ExpandButtonGestureRecognizer);

            _DoubleClickGestureRecognizer.NumberOfTapsRequired = 2;
            _DoubleClickGestureRecognizer.Tapped += DoubleClick;
            _ContentView.GestureRecognizers.Add(_DoubleClickGestureRecognizer);

            _DragGestureRecognizer.DragStarting += OnDragStarting;
            _ContentView.GestureRecognizers.Add(_DragGestureRecognizer);

            _DropGestureRecognizer.Drop += OnDrop;
            _ContentView.GestureRecognizers.Add(_DropGestureRecognizer);

            _MainGrid.SetRow((IView)_ChildrenStackLayout, 1);
            _MainGrid.SetColumn((IView)_ChildrenStackLayout, 0);

            _MainGrid.Children.Add(_ContentStackLayout);
            _MainGrid.Children.Add(_ChildrenStackLayout);
            base.Children.Add(_MainGrid);

            HorizontalOptions = LayoutOptions.Fill;
            VerticalOptions = LayoutOptions.Start;

            Render();
        }

        private void OnDragStarting(object sender, DragStartingEventArgs e)
        {
            if (BindingContext is TreeViewItemBase item)
            {
                e.Data.Properties[EditorDragDrop.DragItemKey] = item.GetModel<object>();
            }
            else if (BindingContext is TreeViewItemGroupBase group)
            {
                e.Data.Properties[EditorDragDrop.DragItemKey] = group.GetModel<object>();
            }
            else
            {
                e.Cancel = true;
            }
        }

        private void OnDrop(object sender, DropEventArgs e)
        {
            if (!e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var source))
            {
                return;
            }

            var target = BindingContext switch
            {
                TreeViewItemBase item => item.GetModel<object>(),
                TreeViewItemGroupBase group => group.GetModel<object>(),
                _ => null
            };

            if (target == null || ReferenceEquals(source, target))
            {
                return;
            }

            var request = new TreeViewDropRequest
            {
                Source = source,
                Target = target
            };

            RequestDrop(request);
            if (request.Handled)
            {
                e.Handled = true;
            }
        }

        private void RequestDrop(TreeViewDropRequest request)
        {
            if (ParentTreeViewItem is not null)
            {
                ParentTreeViewItem.RequestDrop(request);
                return;
            }

            ParentTreeView?.RequestDrop(request);
        }

        private TreeView? FindParentTreeView()
        {
            Element? current = this;
            while (current is not null)
            {
                if (current is TreeView treeView)
                {
                    return treeView;
                }

                current = current.Parent;
            }

            return null;
        }

        public void Select()
        {
            ChildSelected(this);
        }

        private void ChildSelected(TreeViewNode child)
        {
            //Um? How does this work? The method here is a private method so how are we calling it?
            if (ParentTreeViewItem is not null)
            {
                ParentTreeViewItem.ChildSelected(child);
                return;
            }

            ParentTreeView?.ChildSelected(child);
        }

        private void Render()
        {
            _SpacerBoxView.WidthRequest = IndentWidth;

            if ((ChildrenList == null || ChildrenList.Count == 0) && !ShowExpandButtonIfEmpty)
            {
                SetExpandButtonContent(_ExpandButtonTemplate);
                return;
            }

            SetExpandButtonContent(_ExpandButtonTemplate);

            foreach (var item in ChildrenList)
            {
                item.Render();
            }
        }

        /// <summary>
        /// Use DataTemplae 
        /// </summary>
        private void SetExpandButtonContent(DataTemplate expandButtonTemplate)
        {
            if (expandButtonTemplate != null)
            {
                _ExpandButtonContent.Content = (View)expandButtonTemplate.CreateContent();
            }
            else
            {
                _ExpandButtonContent.Content = (View)new ContentView { Content = _EmptyBox };
            }
        }

        private void ExpandButton_Tapped(object sender, EventArgs e)
        {
            _ExpandButtonClickedTime = DateTime.Now;
            IsExpanded = !IsExpanded;
        }

        private void TapGestureRecognizer_Tapped(object sender, EventArgs e)
        {
            //TODO: Hack. We don't want the node to become selected when we are clicking on the expanded button
            if (DateTime.Now - _ExpandButtonClickedTime > new TimeSpan(0, 0, 0, 0, 50))
            {
                ChildSelected(this);
            }
        }

        private void ContextGestureRecognizer_Tapped(object sender, TappedEventArgs e)
        {
            ChildSelected(this);
            ParentTreeView?.RequestContext(this);
        }

        private void DoubleClick(object sender, EventArgs e)
        {
            DoubleClicked?.Invoke(this, new EventArgs());
        }
        private void ItemsSource_CollectionChanged(object sender, NotifyCollectionChangedEventArgs e)
        {
            TreeView.RenderNodes(_Children, _ChildrenStackLayout, e, this);
            Render();
        }
    }
}
