using SailorEditor.Panels;

namespace SailorEditor.Editor.Tests;

public class PanelContractsTests
{
    [Fact]
    public void PanelDescriptor_ExposesSingletonMetadata()
    {
        var descriptor = new PanelDescriptor(
            new PanelTypeId("Hierarchy"),
            "Hierarchy",
            PanelRole.Tool,
            AllowMultipleInstances: false,
            DefaultDockPreference: new PanelDockPreference(DefaultPosition: DockPosition.Left));

        Assert.True(descriptor.IsSingleton);
        Assert.Equal(PanelRole.Tool, descriptor.Role);
        Assert.Equal(DockPosition.Left, descriptor.DefaultDockPreference.DefaultPosition);
    }

    [Fact]
    public void PanelId_New_CreatesNonEmptyValue()
    {
        var panelId = PanelId.New();

        Assert.False(panelId.IsEmpty);
        Assert.NotEqual(string.Empty, panelId.ToString());
    }
}
