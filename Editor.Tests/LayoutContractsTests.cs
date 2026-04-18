using SailorEditor.Layout;
using SailorEditor.Panels;
using System.Text.Json;

namespace SailorEditor.Editor.Tests;

public class LayoutContractsTests
{
    [Fact]
    public void EditorLayout_MinimalGraph_RoundTripsThroughJson()
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

        var json = JsonSerializer.Serialize(layout);
        var restored = JsonSerializer.Deserialize<EditorLayout>(json);

        Assert.NotNull(restored);
        Assert.Equal(1, restored!.Version);
        var tabs = Assert.IsType<TabGroupNode>(restored.Root.Content);
        Assert.Equal(panelId, tabs.ActivePanelId);
        Assert.Single(tabs.Panels);
    }
}
