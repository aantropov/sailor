using SailorEditor.Layout;
using SailorEditor.Panels;

namespace SailorEditor.Editor.Tests;

public class LayoutOperationsTests
{
    [Fact]
    public void CreateDefaultLayout_ContainsCorePanelGroups()
    {
        var layout = LayoutOperations.CreateDefaultLayout();
        var root = Assert.IsType<SplitNode>(layout.Root.Content);

        Assert.Equal(3, root.Children.Count);
        Assert.Equal(3, root.SizeRatios.Count);
    }

    [Fact]
    public void InsertTabbed_AddsPanelToRequestedGroup()
    {
        var panel = new PanelReference(PanelId.New(), new PanelTypeId("AI"));
        var layout = LayoutOperations.CreateDefaultLayout();

        var updated = LayoutOperations.InsertTabbed(layout.Root.Content, "right-inspector", panel);
        var root = Assert.IsType<SplitNode>(updated);
        var tabs = Assert.IsType<TabGroupNode>(root.Children[2]);

        Assert.Contains(tabs.Panels, x => x.PanelId == panel.PanelId);
    }

    [Fact]
    public void HidePanel_RemovesSinglePanelGroupAndCollapsesTree()
    {
        var layout = LayoutOperations.CreateDefaultLayout();
        var root = Assert.IsType<SplitNode>(layout.Root.Content);
        var rightTabs = Assert.IsType<TabGroupNode>(root.Children[2]);

        var updated = LayoutOperations.HidePanel(layout.Root.Content, rightTabs.Panels[0].PanelId);
        var collapsedRoot = Assert.IsType<SplitNode>(updated);

        Assert.Equal(3, collapsedRoot.Children.Count);
        var remainingRightTabs = Assert.IsType<TabGroupNode>(collapsedRoot.Children[2]);
        Assert.Single(remainingRightTabs.Panels);
        Assert.Equal("AI", remainingRightTabs.Panels[0].PanelTypeId.Value);
    }

    [Fact]
    public void Resize_NormalizesRatiosAfterDelta()
    {
        var split = new SplitNode(SplitOrientation.Horizontal, [new TabGroupNode(PanelRole.Tool, [], null), new TabGroupNode(PanelRole.Tool, [], null)], [0.5, 0.5]);

        var resized = LayoutOperations.Resize(split, 0, 0.2);

        Assert.Equal(1.0, resized.SizeRatios.Sum(), 3);
        Assert.True(resized.SizeRatios[0] > resized.SizeRatios[1]);
    }

    [Fact]
    public void ResizeAtPath_UpdatesNestedSplitRatios()
    {
        var nested = new SplitNode(
            SplitOrientation.Vertical,
            [new TabGroupNode(PanelRole.Tool, [], null), new TabGroupNode(PanelRole.Tool, [], null)],
            [0.6, 0.4]);
        var root = new SplitNode(
            SplitOrientation.Horizontal,
            [nested, new TabGroupNode(PanelRole.Tool, [], null)],
            [0.7, 0.3]);

        var updated = Assert.IsType<SplitNode>(LayoutOperations.ResizeAtPath(root, [0], 0, -0.1));
        var updatedNested = Assert.IsType<SplitNode>(updated.Children[0]);

        Assert.Equal([0.5, 0.5], updatedNested.SizeRatios, new DoubleComparer(3));
        Assert.Equal(root.SizeRatios, updated.SizeRatios);
    }

    sealed class DoubleComparer(int precision) : IEqualityComparer<double>
    {
        public bool Equals(double x, double y) => Math.Abs(x - y) < Math.Pow(10, -precision);

        public int GetHashCode(double obj) => obj.GetHashCode();
    }
}
