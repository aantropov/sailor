using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;
using System.Collections.Specialized;
using System.ComponentModel;

namespace SailorEditor.Controls;

public sealed class FrameGraphDiagramView : GraphicsView, IDrawable
{
    const float CardPadding = 10.0f;
    const int VisibleBindingCount = 6;

    FrameGraphFile? frameGraph;
    ObservableList<FrameGraphNode>? subscribedNodes;
    FrameGraphDiagramLayoutResult layout = FrameGraphDiagramLayout.Arrange(0);

    public FrameGraphDiagramView()
    {
        Drawable = this;
        BackgroundColor = Color.FromArgb("#171A1F");
        StartInteraction += OnStartInteraction;
        BindingContextChanged += (_, _) => BindFrameGraph(BindingContext as FrameGraphFile);
        Loaded += (_, _) => BindFrameGraph(BindingContext as FrameGraphFile);
        Unloaded += (_, _) => Detach();
        UpdateLayout();
    }

    public void Draw(ICanvas canvas, RectF dirtyRect)
    {
        canvas.FillColor = Color.FromArgb("#171A1F");
        canvas.FillRectangle(dirtyRect);
        DrawGrid(canvas, dirtyRect);

        if (frameGraph is null || frameGraph.Nodes.Count == 0)
        {
            canvas.FontColor = Color.FromArgb("#929AA5");
            canvas.FontSize = 13.0f;
            canvas.DrawString(
                "No frame nodes. Add a node to start the pipeline.",
                18.0f,
                18.0f,
                440.0f,
                30.0f,
                HorizontalAlignment.Left,
                VerticalAlignment.Center);
            return;
        }

        foreach (var connector in layout.Connectors)
        {
            DrawConnector(canvas, connector);
        }

        foreach (var nodeLayout in layout.Nodes)
        {
            DrawNode(canvas, nodeLayout, frameGraph.Nodes[nodeLayout.Index]);
        }
    }

    void BindFrameGraph(FrameGraphFile? value)
    {
        if (ReferenceEquals(frameGraph, value) && subscribedNodes is not null)
        {
            return;
        }

        Unsubscribe();
        frameGraph = value;
        if (frameGraph is not null)
        {
            frameGraph.PropertyChanged += OnFrameGraphChanged;
            SubscribeNodes(frameGraph.Nodes);
        }
        UpdateLayout();
        Invalidate();
    }

    void Detach()
    {
        Unsubscribe();
        frameGraph = null;
    }

    void SubscribeNodes(ObservableList<FrameGraphNode> nodes)
    {
        subscribedNodes = nodes;
        subscribedNodes.CollectionChanged += OnNodesCollectionChanged;
        subscribedNodes.ItemChanged += OnNodeChanged;
    }

    void Unsubscribe()
    {
        if (frameGraph is not null)
        {
            frameGraph.PropertyChanged -= OnFrameGraphChanged;
        }
        if (subscribedNodes is not null)
        {
            subscribedNodes.CollectionChanged -= OnNodesCollectionChanged;
            subscribedNodes.ItemChanged -= OnNodeChanged;
            subscribedNodes = null;
        }
    }

    void OnFrameGraphChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (frameGraph is not null &&
            args.PropertyName == nameof(FrameGraphFile.Nodes) &&
            !ReferenceEquals(subscribedNodes, frameGraph.Nodes))
        {
            if (subscribedNodes is not null)
            {
                subscribedNodes.CollectionChanged -= OnNodesCollectionChanged;
                subscribedNodes.ItemChanged -= OnNodeChanged;
            }
            SubscribeNodes(frameGraph.Nodes);
            UpdateLayout();
        }

        Invalidate();
    }

    void OnNodesCollectionChanged(object? sender, NotifyCollectionChangedEventArgs args)
    {
        UpdateLayout();
        Invalidate();
    }

    void OnNodeChanged(object? sender, ItemChangedEventArgs<FrameGraphNode> args) =>
        Invalidate();

    void UpdateLayout()
    {
        layout = FrameGraphDiagramLayout.Arrange(frameGraph?.Nodes.Count ?? 0);
        WidthRequest = layout.ContentWidth;
        MinimumWidthRequest = layout.ContentWidth;
        HeightRequest = layout.ContentHeight;
    }

    void OnStartInteraction(object? sender, TouchEventArgs args)
    {
        if (frameGraph is null || args.Touches.Length == 0)
        {
            return;
        }

        var point = args.Touches[0];
        var nodeIndex = layout.HitTest(point.X, point.Y);
        frameGraph.SelectedNode = nodeIndex >= 0 && nodeIndex < frameGraph.Nodes.Count
            ? frameGraph.Nodes[nodeIndex]
            : null;
        Invalidate();
    }

    static void DrawGrid(ICanvas canvas, RectF bounds)
    {
        canvas.StrokeColor = Color.FromArgb("#252A32");
        canvas.StrokeSize = 1.0f;
        const float grid = 24.0f;
        for (var x = 0.0f; x < bounds.Right; x += grid)
        {
            canvas.DrawLine(x, bounds.Top, x, bounds.Bottom);
        }
        for (var y = 0.0f; y < bounds.Bottom; y += grid)
        {
            canvas.DrawLine(bounds.Left, y, bounds.Right, y);
        }
    }

    static void DrawConnector(
        ICanvas canvas,
        FrameGraphDiagramConnector connector)
    {
        var start = new PointF((float)connector.Start.X, (float)connector.Start.Y);
        var end = new PointF((float)connector.End.X, (float)connector.End.Y);
        canvas.StrokeColor = Color.FromArgb("#7E8B9D");
        canvas.StrokeSize = 2.0f;
        canvas.DrawLine(start, end);

        const float arrow = 8.0f;
        canvas.DrawLine(end, new PointF(end.X - arrow, end.Y - arrow * 0.55f));
        canvas.DrawLine(end, new PointF(end.X - arrow, end.Y + arrow * 0.55f));
    }

    void DrawNode(
        ICanvas canvas,
        FrameGraphDiagramNodeLayout nodeLayout,
        FrameGraphNode node)
    {
        var bounds = nodeLayout.Bounds;
        var rect = new RectF(
            (float)bounds.X,
            (float)bounds.Y,
            (float)bounds.Width,
            (float)bounds.Height);
        var isSelected = ReferenceEquals(frameGraph?.SelectedNode, node);

        canvas.FillColor = isSelected
            ? Color.FromArgb("#263C57")
            : Color.FromArgb("#20252C");
        canvas.FillRoundedRectangle(rect, 7.0f);
        canvas.StrokeColor = isSelected
            ? Color.FromArgb("#74A5E8")
            : Color.FromArgb("#56606E");
        canvas.StrokeSize = isSelected ? 2.0f : 1.0f;
        canvas.DrawRoundedRectangle(rect, 7.0f);

        canvas.FontColor = Color.FromArgb("#8FA2B8");
        canvas.FontSize = 11.0f;
        canvas.DrawString(
            $"FRAME {nodeLayout.Index + 1}",
            rect.X + CardPadding,
            rect.Y + 7.0f,
            rect.Width - CardPadding * 2,
            18.0f,
            HorizontalAlignment.Left,
            VerticalAlignment.Center);

        canvas.FontColor = Colors.White;
        canvas.FontSize = 14.0f;
        canvas.DrawString(
            string.IsNullOrWhiteSpace(node.Name) ? "Unnamed Node" : node.Name,
            rect.X + CardPadding,
            rect.Y + 27.0f,
            rect.Width - CardPadding * 2,
            24.0f,
            HorizontalAlignment.Left,
            VerticalAlignment.Center);

        canvas.FontColor = Color.FromArgb("#929AA5");
        canvas.FontSize = 10.0f;
        canvas.DrawString(
            string.IsNullOrWhiteSpace(node.Tag)
                ? "tag: node name"
                : $"tag: {node.Tag}",
            rect.X + CardPadding,
            rect.Y + 48.0f,
            rect.Width - CardPadding * 2,
            17.0f,
            HorizontalAlignment.Left,
            VerticalAlignment.Center);

        canvas.StrokeColor = Color.FromArgb("#3B424D");
        canvas.StrokeSize = 1.0f;
        canvas.DrawLine(
            rect.X + CardPadding,
            rect.Y + 70.0f,
            rect.Right - CardPadding,
            rect.Y + 70.0f);

        canvas.FontSize = 11.0f;
        var bindingY = rect.Y + 78.0f;
        var visibleBindings = node.RenderTargets.Take(VisibleBindingCount).ToArray();
        foreach (var binding in visibleBindings)
        {
            canvas.FontColor = Color.FromArgb("#9BB9DC");
            canvas.DrawString(
                binding.Key ?? string.Empty,
                rect.X + CardPadding,
                bindingY,
                78.0f,
                20.0f,
                HorizontalAlignment.Left,
                VerticalAlignment.Center);
            canvas.FontColor = Color.FromArgb("#D8DEE8");
            canvas.DrawString(
                $"→ {binding.Value}",
                rect.X + 88.0f,
                bindingY,
                rect.Width - 98.0f,
                20.0f,
                HorizontalAlignment.Left,
                VerticalAlignment.Center);
            bindingY += 22.0f;
        }

        if (node.RenderTargets.Count > visibleBindings.Length)
        {
            canvas.FontColor = Color.FromArgb("#929AA5");
            canvas.DrawString(
                $"+{node.RenderTargets.Count - visibleBindings.Length} bindings",
                rect.X + CardPadding,
                bindingY,
                rect.Width - CardPadding * 2,
                20.0f,
                HorizontalAlignment.Left,
                VerticalAlignment.Center);
        }
        else if (node.RenderTargets.Count == 0)
        {
            canvas.FontColor = Color.FromArgb("#747D89");
            canvas.DrawString(
                "No resource bindings",
                rect.X + CardPadding,
                bindingY,
                rect.Width - CardPadding * 2,
                20.0f,
                HorizontalAlignment.Left,
                VerticalAlignment.Center);
        }

        canvas.FontColor = Color.FromArgb("#929AA5");
        canvas.FontSize = 10.5f;
        canvas.DrawString(
            $"{node.Strings.Count} strings  ·  {node.Floats.Count} floats  ·  {node.Vec4.Count} vec4",
            rect.X + CardPadding,
            rect.Bottom - 31.0f,
            rect.Width - CardPadding * 2,
            20.0f,
            HorizontalAlignment.Left,
            VerticalAlignment.Center);
    }
}
