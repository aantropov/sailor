using SailorEditor.Panels;

namespace SailorEditor.Layout;

public static class LayoutOperations
{
    public static EditorLayout CreateDefaultLayout()
    {
        var contentPanel = new PanelReference(PanelId.New(), new PanelTypeId("Content"));
        var hierarchyPanel = new PanelReference(PanelId.New(), new PanelTypeId("Hierarchy"));
        var inspectorPanel = new PanelReference(PanelId.New(), new PanelTypeId("Inspector"));
        var scenePanel = new PanelReference(PanelId.New(), new PanelTypeId("Scene"));
        var consolePanel = new PanelReference(PanelId.New(), new PanelTypeId("Console"));

        return new EditorLayout(
            1,
            new LayoutRoot(
                new SplitNode(
                    SplitOrientation.Horizontal,
                    [
                        new SplitNode(
                            SplitOrientation.Vertical,
                            [
                                new TabGroupNode(PanelRole.Tool, [contentPanel], contentPanel.PanelId, "left-top"),
                                new TabGroupNode(PanelRole.Tool, [hierarchyPanel], hierarchyPanel.PanelId, "left-bottom")
                            ],
                            [0.5, 0.5],
                            MinSizes: [180, 180]),
                        new SplitNode(
                            SplitOrientation.Vertical,
                            [
                                new TabGroupNode(PanelRole.Document, [scenePanel], scenePanel.PanelId, "center-docs"),
                                new TabGroupNode(PanelRole.Tool, [consolePanel], consolePanel.PanelId, "bottom-console")
                            ],
                            [0.72, 0.28],
                            MinSizes: [280, 140]),
                        new TabGroupNode(PanelRole.Tool, [inspectorPanel], inspectorPanel.PanelId, "right-inspector")
                    ],
                    [0.22, 0.56, 0.22],
                    MinSizes: [220, 320, 220])),
            new WindowBounds(80, 80, 1600, 900));
    }

    public static TabGroupNode AddPanelToGroup(TabGroupNode group, PanelReference panel, bool activate = true)
    {
        var panels = group.Panels.Where(x => x.PanelId != panel.PanelId).Append(panel).ToArray();
        return group with { Panels = panels, ActivePanelId = activate ? panel.PanelId : group.ActivePanelId ?? panel.PanelId };
    }

    public static LayoutNode ReplaceNode(LayoutNode root, Func<LayoutNode, bool> predicate, Func<LayoutNode, LayoutNode> replace)
    {
        if (predicate(root))
            return replace(root);

        return root switch
        {
            SplitNode split => split with { Children = split.Children.Select(child => ReplaceNode(child, predicate, replace)).ToArray() },
            _ => root
        };
    }

    public static LayoutNode InsertTabbed(LayoutNode root, string groupId, PanelReference panel)
        => ReplaceNode(root, node => node is TabGroupNode tabs && tabs.GroupId == groupId, node => AddPanelToGroup((TabGroupNode)node, panel));

    public static LayoutNode HidePanel(LayoutNode root, PanelId panelId) => Cleanup(RemovePanel(root, panelId));

    public static LayoutNode RemovePanel(LayoutNode root, PanelId panelId)
    {
        return root switch
        {
            TabGroupNode tabs => RemoveFromGroup(tabs, panelId),
            SplitNode split => split with { Children = split.Children.Select(x => RemovePanel(x, panelId)).OfType<LayoutNode>().ToArray() },
            _ => root
        };
    }

    static LayoutNode RemoveFromGroup(TabGroupNode tabs, PanelId panelId)
    {
        var panels = tabs.Panels.Where(x => x.PanelId != panelId).ToArray();
        if (panels.Length == 0)
            return EmptyNode.Instance;

        return tabs with { Panels = panels, ActivePanelId = panels.Any(x => x.PanelId == tabs.ActivePanelId) ? tabs.ActivePanelId : panels[0].PanelId };
    }

    static LayoutNode Cleanup(LayoutNode node)
    {
        return node switch
        {
            SplitNode split => CleanupSplit(split),
            _ => node
        };
    }

    static LayoutNode CleanupSplit(SplitNode split)
    {
        var children = split.Children.Select(Cleanup).Where(x => x is not EmptyNode).ToArray();
        if (children.Length == 1)
            return children[0];

        var ratios = NormalizeRatios(split.SizeRatios, children.Length);
        return split with { Children = children, SizeRatios = ratios };
    }

    public static SplitNode Resize(SplitNode split, int leadingIndex, double delta, double minRatio = 0.1)
    {
        if (leadingIndex < 0 || leadingIndex >= split.SizeRatios.Count - 1)
            return split;

        var ratios = split.SizeRatios.ToArray();
        var left = Math.Clamp(ratios[leadingIndex] + delta, minRatio, 1 - minRatio);
        var right = Math.Clamp(ratios[leadingIndex + 1] - delta, minRatio, 1 - minRatio);
        var sum = left + right;
        ratios[leadingIndex] = left / sum * (split.SizeRatios[leadingIndex] + split.SizeRatios[leadingIndex + 1]);
        ratios[leadingIndex + 1] = right / sum * (split.SizeRatios[leadingIndex] + split.SizeRatios[leadingIndex + 1]);
        return split with { SizeRatios = NormalizeRatios(ratios, ratios.Length) };
    }

    public static double[] NormalizeRatios(IReadOnlyList<double> ratios, int count)
    {
        var values = ratios.Take(count).Select(x => x <= 0 ? 1d : x).ToArray();
        if (values.Length < count)
            values = values.Concat(Enumerable.Repeat(1d, count - values.Length)).ToArray();

        var sum = values.Sum();
        return values.Select(x => x / sum).ToArray();
    }

    sealed record EmptyNode() : LayoutNode
    {
        public static readonly EmptyNode Instance = new();
    }
}
