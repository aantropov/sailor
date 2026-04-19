using SailorEditor.Layout;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.State;

namespace Editor.Tests;

public sealed class ShellStateConsistencyTests
{
    [Fact]
    public void ResetPanels_ClearsLookupAndPanelFocusedState()
    {
        var shellState = new ShellState();
        var panel = CreatePanel(new PanelId("scene-panel"), KnownPanelTypes.Scene, PanelRole.Document, "center-docs");

        shellState.OpenPanel(panel, panel.GroupId);
        shellState.FocusViewport("scene:1", "scene:1", "scene:1");

        shellState.ResetPanels();

        Assert.Empty(shellState.OpenPanels);
        Assert.False(shellState.IsOpen(panel.PanelId));
        Assert.False(shellState.TryGetPanel(panel.PanelId, out _));
        Assert.Null(shellState.Focus.FocusedPanelId);
        Assert.Null(shellState.Focus.ActiveTabGroupId);
        Assert.Null(shellState.Focus.ActiveDocumentPanelId);
        Assert.Equal("scene:1", shellState.Focus.FocusedViewportId);
        Assert.Equal("scene:1", shellState.Focus.SelectionOwner);
        Assert.Equal("scene:1", shellState.Focus.KeyboardInputOwner);
    }

    [Fact]
    public async Task ApplyLayout_RebuildsLookupFromLayoutWithoutLeavingClosedPanelsReachable()
    {
        var panelRegistry = new PanelRegistry();
        var shellState = new ShellState();
        var shellHost = new EditorShellHost(panelRegistry, new InMemoryLayoutStore(), shellState);

        var scenePanelId = new PanelId("scene-panel");
        var consolePanelId = new PanelId("console-panel");
        var initialLayout = new EditorLayout(
            1,
            new LayoutRoot(
                new SplitNode(
                    SplitOrientation.Vertical,
                    [
                        new TabGroupNode(PanelRole.Document, [new PanelReference(scenePanelId, KnownPanelTypes.Scene)], scenePanelId, "center-docs"),
                        new TabGroupNode(PanelRole.Tool, [new PanelReference(consolePanelId, KnownPanelTypes.Console)], consolePanelId, "bottom-console")
                    ],
                    [0.7, 0.3])));

        shellHost.ApplyLayout(initialLayout);
        Assert.True(shellState.TryGetPanel(consolePanelId, out _));

        await shellHost.ClosePanelAsync(consolePanelId);

        Assert.False(shellState.IsOpen(consolePanelId));
        Assert.False(shellState.TryGetPanel(consolePanelId, out _));
        Assert.DoesNotContain(shellState.OpenPanels, x => x.PanelId == consolePanelId);

        await shellHost.FocusPanelAsync(consolePanelId);

        Assert.Equal(scenePanelId, shellState.Focus.FocusedPanelId);
        Assert.Equal(scenePanelId, shellState.Focus.ActiveDocumentPanelId);
    }

    [Fact]
    public async Task ResizeSplitAsync_UpdatesLayoutRatiosAndPersists()
    {
        var panelRegistry = new PanelRegistry();
        var shellState = new ShellState();
        var layoutStore = new InMemoryLayoutStore();
        var shellHost = new EditorShellHost(panelRegistry, layoutStore, shellState);

        var leftPanelId = new PanelId("left-panel");
        var rightPanelId = new PanelId("right-panel");
        var initialLayout = new EditorLayout(
            1,
            new LayoutRoot(
                new SplitNode(
                    SplitOrientation.Horizontal,
                    [
                        new TabGroupNode(PanelRole.Tool, [new PanelReference(leftPanelId, KnownPanelTypes.Content)], leftPanelId, "left"),
                        new TabGroupNode(PanelRole.Tool, [new PanelReference(rightPanelId, KnownPanelTypes.Inspector)], rightPanelId, "right")
                    ],
                    [0.5, 0.5])));

        shellHost.ApplyLayout(initialLayout);

        await shellHost.ResizeSplitAsync([], 0, 0.15);

        var updated = Assert.IsType<SplitNode>(shellHost.CurrentLayout!.Root.Content);
        Assert.Equal(0.65, updated.SizeRatios[0], 3);
        Assert.Equal(0.35, updated.SizeRatios[1], 3);
        Assert.NotNull(layoutStore.Layout);
    }

    static PanelInstance CreatePanel(PanelId panelId, PanelTypeId panelTypeId, PanelRole role, string groupId)
        => new(panelId, panelTypeId, panelTypeId.Value, role, new Microsoft.Maui.Controls.Label(), true, groupId);

    sealed class InMemoryLayoutStore : IEditorShellLayoutStore
    {
        public EditorLayout? Layout { get; private set; }

        public ValueTask<EditorLayout?> LoadAsync(CancellationToken cancellationToken = default) => ValueTask.FromResult(Layout);

        public ValueTask SaveAsync(EditorLayout layout, CancellationToken cancellationToken = default)
        {
            Layout = layout;
            return ValueTask.CompletedTask;
        }
    }
}
