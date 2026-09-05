using SailorEditor.Workflow;

namespace SailorEditor.Tests;

public sealed class FrameGraphDiagramLayoutTests
{
    [Fact]
    public void Arrange_PreservesFrameOrderAndConnectsAdjacentNodes()
    {
        var layout = FrameGraphDiagramLayout.Arrange(3);

        Assert.Equal(3, layout.Nodes.Count);
        Assert.Equal(2, layout.Connectors.Count);
        Assert.Equal(new[] { 0, 1, 2 }, layout.Nodes.Select(node => node.Index));
        Assert.True(layout.Nodes[0].Bounds.Right < layout.Nodes[1].Bounds.X);
        Assert.True(layout.Nodes[1].Bounds.Right < layout.Nodes[2].Bounds.X);
        Assert.Equal(0, layout.Connectors[0].FromIndex);
        Assert.Equal(1, layout.Connectors[0].ToIndex);
        Assert.Equal(layout.Nodes[0].Bounds.Right, layout.Connectors[0].Start.X);
        Assert.Equal(layout.Nodes[1].Bounds.X, layout.Connectors[0].End.X);
        Assert.True(layout.ContentWidth > FrameGraphDiagramLayout.MinimumWidth);
    }

    [Fact]
    public void HitTest_ReturnsCardIndexAndRejectsConnectorSpace()
    {
        var layout = FrameGraphDiagramLayout.Arrange(2);
        var first = layout.Nodes[0].Bounds;
        var second = layout.Nodes[1].Bounds;

        Assert.Equal(
            0,
            layout.HitTest(
                first.X + first.Width * 0.5,
                first.Y + first.Height * 0.5));
        Assert.Equal(
            1,
            layout.HitTest(
                second.X + second.Width * 0.5,
                second.Y + second.Height * 0.5));
        Assert.Equal(-1, layout.HitTest(first.Right + 1.0, first.Y));
    }

    [Fact]
    public void Arrange_UsesStableEmptyStateAndRejectsNegativeCounts()
    {
        var layout = FrameGraphDiagramLayout.Arrange(0);

        Assert.Empty(layout.Nodes);
        Assert.Empty(layout.Connectors);
        Assert.Equal(FrameGraphDiagramLayout.MinimumWidth, layout.ContentWidth);
        Assert.Equal(-1, layout.HitTest(20.0, 20.0));
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            FrameGraphDiagramLayout.Arrange(-1));
    }

    [Fact]
    public void Move_ReordersByReferenceAndClampsAtPipelineEnds()
    {
        var first = new object();
        var second = new object();
        var third = new object();
        IList<object> nodes = new List<object> { first, second, third };

        Assert.True(FrameGraphDiagramOrder.Move(nodes, second, 1));
        Assert.Equal(new[] { first, third, second }, nodes);
        Assert.False(FrameGraphDiagramOrder.Move(nodes, second, 1));
        Assert.True(FrameGraphDiagramOrder.Move(nodes, second, -2));
        Assert.Equal(new[] { second, first, third }, nodes);
        Assert.False(FrameGraphDiagramOrder.Move(nodes, new object(), 1));
    }

}
