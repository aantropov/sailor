using SailorEditor.Services;

namespace SailorEditor.Controls;

public partial class VerticalGridSplitterControl : ContentView
{
    public VerticalGridSplitterControl()
    {
        InitializeComponent();
        SetupGestureRecognizers();
    }

    private void SetupGestureRecognizers()
    {
        var dragGestureRecognizer = new DragGestureRecognizer();
        dragGestureRecognizer.DragStarting += OnDragStarting;
        Splitter.GestureRecognizers.Add(dragGestureRecognizer);

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

    private void OnDragStarting(object sender, DragStartingEventArgs e)
    {
        e.Data.Properties.Add("Source", Splitter);
        Splitter.IsVisible = false;

        EngineService.ShowMainWindow(false);
    }

    private void OnDragOver(object sender, DragEventArgs e)
    {
        if (e.Data.Properties.ContainsKey("Source") && e.Data.Properties["Source"] == Splitter)
        {
            UpdateSplitterPosition(sender, e);
            Splitter.IsVisible = true;
        }
    }

    private void OnDragCompleted(object sender, DropCompletedEventArgs e)
    {
        Splitter.IsVisible = true;

        EngineService.ShowMainWindow(true);
    }

    public void UpdateSplitterPosition(object sender, DragEventArgs e)
    {
        var parentGrid = Parent as Grid;
        if (parentGrid != null)
        {
            int columnIndex = Grid.GetColumn(this);
            columnIndex--;

            var newPosition = e.GetPosition(parentGrid).Value.X;
            double offset = 0;

            for (int i = 0; i < columnIndex; i++)
            {
                offset += parentGrid.ColumnDefinitions[i].Width.Value;
            }

            var newWidth = newPosition - offset;

            if (DeviceInfo.Platform == DevicePlatform.WinUI)
            {
                e.PlatformArgs.DragEventArgs.DragUIOverride!.IsCaptionVisible = false;
                e.PlatformArgs.DragEventArgs.DragUIOverride!.IsGlyphVisible = false;
            }

            Splitter.IsVisible = true;

            if (columnIndex < parentGrid.ColumnDefinitions.Count)
            {
                newWidth = Math.Max(newWidth, 20);

                parentGrid.ColumnDefinitions[columnIndex].Width = new GridLength(newWidth);
            }
        }
    }
}