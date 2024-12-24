using SailorEditor.Services;

namespace SailorEditor.Controls;

public partial class HorizontalGridSplitterControl : ContentView
{
    public HorizontalGridSplitterControl()
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
            int rowIndex = Grid.GetRow(this);
            rowIndex--;

            var newPosition = e.GetPosition(parentGrid).Value.Y;
            double offset = 0;

            for (int i = 0; i < rowIndex; i++)
            {
                offset += parentGrid.RowDefinitions[i].Height.Value;
            }

            var newHeight = newPosition - offset;

            if (DeviceInfo.Platform == DevicePlatform.WinUI)
            {
                e.PlatformArgs.DragEventArgs.DragUIOverride!.IsCaptionVisible = false;
                e.PlatformArgs.DragEventArgs.DragUIOverride!.IsGlyphVisible = false;
            }

            Splitter.IsVisible = true;

            if (rowIndex < parentGrid.RowDefinitions.Count)
            {
                newHeight = Math.Max(newHeight, 20);

                parentGrid.RowDefinitions[rowIndex].Height = new GridLength(newHeight);
            }
        }
    }
}