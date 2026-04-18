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

        Assert.Equal(2, collapsedRoot.Children.Count);
    }

    [Fact]
    public void Resize_NormalizesRatiosAfterDelta()
    {
        var split = new SplitNode(SplitOrientation.Horizontal, [new TabGroupNode(PanelRole.Tool, [], null), new TabGroupNode(PanelRole.Tool, [], null)], [0.5, 0.5]);

        var resized = LayoutOperations.Resize(split, 0, 0.2);

        Assert.Equal(1.0, resized.SizeRatios.Sum(), 3);
        Assert.True(resized.SizeRatios[0] > resized.SizeRatios[1]);
    }
}
