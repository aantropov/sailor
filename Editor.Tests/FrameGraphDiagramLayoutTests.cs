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

    [Fact]
    public void RendererInspector_UsesDiagramSelectionWithoutChangingYamlSchema()
    {
        var repositoryRoot = FindRepositoryRoot();
        var inspector = File.ReadAllText(Path.Combine(
            repositoryRoot,
            "Editor",
            "Views",
            "InspectorView",
            "FrameGraphFileTemplate.xaml"));
        var viewModel = File.ReadAllText(Path.Combine(
            repositoryRoot,
            "Editor",
            "ViewModels",
            "FrameGraphFile.cs"));
        var diagram = File.ReadAllText(Path.Combine(
            repositoryRoot,
            "Editor",
            "Controls",
            "FrameGraphDiagramView.cs"));

        Assert.Contains("FrameGraphDiagramView", inspector, StringComparison.Ordinal);
        Assert.Contains("SelectedNode", inspector, StringComparison.Ordinal);
        Assert.Contains("MoveNodeEarlierCommand", inspector, StringComparison.Ordinal);
        Assert.Contains("MoveNodeLaterCommand", inspector, StringComparison.Ordinal);
        Assert.Contains("Global Vec4", inspector, StringComparison.Ordinal);
        Assert.Contains("frameGraph.SelectedNode", diagram, StringComparison.Ordinal);
        Assert.Contains("{ \"vec4\", WriteNamedVec4List(Vec4) }", viewModel, StringComparison.Ordinal);
        Assert.Contains("Tag = ReadString(node, \"tag\")", viewModel, StringComparison.Ordinal);
        Assert.Contains("node.Add(\"tag\", Tag)", viewModel, StringComparison.Ordinal);
        Assert.Contains("Value.PropertyChanged += OnComponentChanged", viewModel, StringComparison.Ordinal);
        Assert.DoesNotContain("editorX", viewModel, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("editorY", viewModel, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("editor", viewModel[viewModel.IndexOf("void SaveRendererAsset()", StringComparison.Ordinal)..viewModel.IndexOf("void TrackList", StringComparison.Ordinal)], StringComparison.OrdinalIgnoreCase);
    }

    static string FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "CMakeLists.txt")) &&
                Directory.Exists(Path.Combine(directory.FullName, "Editor")))
            {
                return directory.FullName;
            }
            directory = directory.Parent;
        }

        throw new DirectoryNotFoundException("Could not locate the Sailor repository root.");
    }
}
