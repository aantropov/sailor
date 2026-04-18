using SailorEditor.Layout;
using SailorEditor.Panels;

namespace SailorEditor.Editor.Tests;

public class LayoutContractsTests
{
    [Fact]
    public void EditorLayout_MinimalGraph_PreservesExpectedStructure()
    {
        var panelId = PanelId.New();
        var layout = new EditorLayout(
            Version: 1,
            Root: new LayoutRoot(
                new TabGroupNode(
                    PanelRole.Tool,
                    [new PanelReference(panelId, new PanelTypeId("Hierarchy"))],
                    ActivePanelId: panelId,
                    GroupId: "left-tools")),
            MainWindow: new WindowBounds(10, 20, 1600, 900));

        Assert.Equal(1, layout.Version);
        var tabs = Assert.IsType<TabGroupNode>(layout.Root.Content);
        Assert.Equal(panelId, tabs.ActivePanelId);
        Assert.Single(tabs.Panels);
        Assert.Equal("Hierarchy", tabs.Panels[0].PanelTypeId.Value);
    }
}
