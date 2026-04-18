using SailorEditor.Panels;

namespace SailorEditor.Layout;

public sealed record EditorLayout(
    int Version,
    LayoutRoot Root,
    WindowBounds? MainWindow = null,
    IReadOnlyList<FloatingWindowNode>? FloatingWindows = null);

public sealed record LayoutRoot(LayoutNode Content);

public abstract record LayoutNode;

public enum SplitOrientation
{
    Horizontal,
    Vertical,
}

public sealed record SplitNode(
    SplitOrientation Orientation,
    IReadOnlyList<LayoutNode> Children,
    IReadOnlyList<double> SizeRatios,
    IReadOnlyList<double>? MinSizes = null,
    IReadOnlyList<double>? MaxSizes = null) : LayoutNode;

public sealed record TabGroupNode(
    PanelRole Role,
    IReadOnlyList<PanelReference> Panels,
    PanelId? ActivePanelId = null,
    string? GroupId = null) : LayoutNode;

public sealed record FloatingWindowNode(
    WindowBounds Bounds,
    LayoutNode Content,
    string? WindowId = null);

public sealed record PanelReference(
    PanelId PanelId,
    PanelTypeId PanelTypeId,
    string? TitleOverride = null,
    string? PersistenceKey = null);

public sealed record WindowBounds(double X, double Y, double Width, double Height);
