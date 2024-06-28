namespace SailorEditor.Controls;
public partial class GridSplitterControl : ContentView
{
    public GridSplitterControl()
    {
        InitializeComponent();
        SetupGestureRecognizers();
    }

    private void SetupGestureRecognizers()
    {
        var dragGestureRecognizer = new DragGestureRecognizer();
        dragGestureRecognizer.DragStarting += OnDragStarting;
        splitter.GestureRecognizers.Add(dragGestureRecognizer);

        PropertyChanged += (s, e) =>
        {
            if (e.PropertyName == "Parent" && this.Parent is Grid parentGrid)
            {
                var dropGestureRecognizer = new DropGestureRecognizer();
                dropGestureRecognizer.DragOver += OnDragOver;
                parentGrid.GestureRecognizers.Add(dropGestureRecognizer);
            }
        };
    }

    private void OnDragStarting(object sender, DragStartingEventArgs e) => splitter.IsVisible = false;

    private void OnDragOver(object sender, DragEventArgs e) => UpdateSplitterPosition(sender, e);

    public void UpdateSplitterPosition(object sender, DragEventArgs e)
    {
        int columnIndex = Grid.GetColumn(this);
        columnIndex--;

        var newWidth = (int)e.GetPosition(null)!.Value.X;

        if (DeviceInfo.Platform == DevicePlatform.WinUI)
        {
            e.PlatformArgs.DragEventArgs.DragUIOverride!.IsCaptionVisible = false;
            e.PlatformArgs.DragEventArgs.DragUIOverride!.IsGlyphVisible = false;
        }

        splitter.IsVisible = true;

        var parentGrid = Parent as Grid;

        if (parentGrid != null && columnIndex < parentGrid.ColumnDefinitions.Count)
        {
            parentGrid.ColumnDefinitions[columnIndex].Width = new GridLength(newWidth);
        }
    }
}