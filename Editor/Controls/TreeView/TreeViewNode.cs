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
        private readonly ContentView _ContentView = new() { HorizontalOptions = LayoutOptions.Fill, };
        private readonly StackLayout _ChildrenStackLayout = new()
        {
            Orientation = StackOrientation.Vertical,
            Spacing = 0,
            IsVisible = false
        };

        private IList<TreeViewNode> _Children = new ObservableCollection<TreeViewNode>();
        private readonly TapGestureRecognizer _TapGestureRecognizer = new();
        private readonly TapGestureRecognizer _ExpandButtonGestureRecognizer = new();
        private readonly TapGestureRecognizer _DoubleClickGestureRecognizer = new();

        internal readonly BoxView SelectionBoxView = new() { Color = Colors.Blue, Opacity = .5, IsVisible = false };

        private TreeView ParentTreeView => Parent?.Parent as TreeView;
        private double IndentWidth => Depth * SpacerWidth;
        private int SpacerWidth { get; } = 30;
        private int Depth => ParentTreeViewItem?.Depth + 1 ?? 0;

        private bool _ShowExpandButtonIfEmpty = false;
        private Color _SelectedBackgroundColor = Colors.Blue;
        private double _SelectedBackgroundOpacity = .3;

        public event EventHandler Expanded;

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
                _ChildrenStackLayout.IsVisible = value;

                Render();
                if (value)
                {
                    Expanded?.Invoke(this, new EventArgs());
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
            var itemsSource = (ObservableCollection<TreeViewNode>)_Children;
            itemsSource.CollectionChanged += ItemsSource_CollectionChanged;

            _TapGestureRecognizer.Tapped += TapGestureRecognizer_Tapped;
            GestureRecognizers.Add(_TapGestureRecognizer);

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

            _MainGrid.SetRow((IView)_ChildrenStackLayout, 1);
            _MainGrid.SetColumn((IView)_ChildrenStackLayout, 0);

            _MainGrid.Children.Add(_ContentStackLayout);
            _MainGrid.Children.Add(_ChildrenStackLayout);

            base.Children.Add(_MainGrid);

            HorizontalOptions = LayoutOptions.Fill;
            VerticalOptions = LayoutOptions.Start;

            Render();
        }

        private void ChildSelected(TreeViewNode child)
        {
            //Um? How does this work? The method here is a private method so how are we calling it?
            ParentTreeViewItem?.ChildSelected(child);
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
