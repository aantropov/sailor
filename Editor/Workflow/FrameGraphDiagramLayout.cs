namespace SailorEditor.Workflow;

public readonly record struct FrameGraphDiagramPoint(double X, double Y);

public readonly record struct FrameGraphDiagramRect(
    double X,
    double Y,
    double Width,
    double Height)
{
    public double Right => X + Width;
    public double Bottom => Y + Height;

    public bool Contains(double x, double y) =>
        x >= X && x <= Right && y >= Y && y <= Bottom;
}

public readonly record struct FrameGraphDiagramNodeLayout(
    int Index,
    FrameGraphDiagramRect Bounds);

public readonly record struct FrameGraphDiagramConnector(
    int FromIndex,
    int ToIndex,
    FrameGraphDiagramPoint Start,
    FrameGraphDiagramPoint End);

public sealed record FrameGraphDiagramLayoutResult(
    IReadOnlyList<FrameGraphDiagramNodeLayout> Nodes,
    IReadOnlyList<FrameGraphDiagramConnector> Connectors,
    double ContentWidth,
    double ContentHeight)
{
    public int HitTest(double x, double y)
    {
        for (var index = Nodes.Count - 1; index >= 0; --index)
        {
            if (Nodes[index].Bounds.Contains(x, y))
            {
                return Nodes[index].Index;
            }
        }

        return -1;
    }
}

public static class FrameGraphDiagramLayout
{
    public const double NodeWidth = 220.0;
    public const double NodeHeight = 270.0;
    public const double NodeGap = 58.0;
    public const double Padding = 18.0;
    public const double MinimumWidth = 480.0;

    public static FrameGraphDiagramLayoutResult Arrange(int nodeCount)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(nodeCount);

        var nodes = new List<FrameGraphDiagramNodeLayout>(nodeCount);
        var connectors = new List<FrameGraphDiagramConnector>(Math.Max(0, nodeCount - 1));
        for (var index = 0; index < nodeCount; ++index)
        {
            var x = Padding + index * (NodeWidth + NodeGap);
            var bounds = new FrameGraphDiagramRect(
                x,
                Padding,
                NodeWidth,
                NodeHeight);
            nodes.Add(new FrameGraphDiagramNodeLayout(index, bounds));

            if (index == 0)
            {
                continue;
            }

            var previous = nodes[index - 1].Bounds;
            connectors.Add(new FrameGraphDiagramConnector(
                index - 1,
                index,
                new FrameGraphDiagramPoint(previous.Right, previous.Y + previous.Height * 0.5),
                new FrameGraphDiagramPoint(bounds.X, bounds.Y + bounds.Height * 0.5)));
        }

        var contentWidth = nodeCount == 0
            ? MinimumWidth
            : Math.Max(
                MinimumWidth,
                Padding * 2 + nodeCount * NodeWidth + (nodeCount - 1) * NodeGap);
        return new FrameGraphDiagramLayoutResult(
            nodes,
            connectors,
            contentWidth,
            Padding * 2 + NodeHeight);
    }
}

public static class FrameGraphDiagramOrder
{
    public static bool Move<T>(IList<T> items, T? item, int offset)
        where T : class
    {
        ArgumentNullException.ThrowIfNull(items);
        if (item is null || offset == 0)
        {
            return false;
        }

        var index = items.IndexOf(item);
        if (index < 0)
        {
            return false;
        }

        var destination = Math.Clamp(index + offset, 0, items.Count - 1);
        if (destination == index)
        {
            return false;
        }

        items.RemoveAt(index);
        items.Insert(destination, item);
        return true;
    }
}
