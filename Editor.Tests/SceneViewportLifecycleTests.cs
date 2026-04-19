using SailorEditor.Scene;
using SailorEditor.Services;
using SailorEditor.State;
using SailorEditor.Panels;

namespace Editor.Tests;

public sealed class SceneViewportLifecycleTests
{
    [Fact]
    public void Sync_BindsHost_UpdatesViewport_AndTracksRenderTarget()
    {
        var backend = new FakeSceneViewportBackend();
        var sut = new SceneViewportLifecycleAdapter(backend, 7);

        var updated = sut.Sync(new SceneViewportFrame(
            new SceneViewportRect(10, 20, 1280, 720),
            new SceneViewportRect(0, 0, 800, 600),
            new SceneViewportRenderTarget(1600, 1200),
            true,
            true,
            (nint)0xCAFE));

        Assert.True(updated);
        Assert.Equal([(ulong)7], backend.UpdatedViewportIds);
        Assert.Equal((ulong)7, backend.BoundViewportId);
        Assert.Equal((nint)0xCAFE, backend.BoundHostHandle);
        Assert.Equal(new SceneViewportRect(0, 0, 800, 600), backend.LastEditorViewport);
        Assert.Equal(new SceneViewportRenderTarget(1600, 1200), backend.LastRenderTarget);
    }

    [Fact]
    public void Sync_DoesNotReapplyStableRenderTargetOrHostBinding()
    {
        var backend = new FakeSceneViewportBackend();
        var sut = new SceneViewportLifecycleAdapter(backend, 7);
        var frame = new SceneViewportFrame(new SceneViewportRect(0, 0, 640, 480), new SceneViewportRect(0, 0, 640, 480), new SceneViewportRenderTarget(640, 480), true, false, (nint)42);

        sut.Sync(frame);
        sut.Sync(frame);

        Assert.Equal(1, backend.BindCount);
        Assert.Equal(1, backend.RenderTargetSetCount);
        Assert.Equal(2, backend.UpdatedViewportIds.Count);
    }

    [Fact]
    public void FocusCoordinator_ClaimsAndReleasesViewportOwnership()
    {
        var shellState = new ShellState();
        shellState.FocusPanel(new PanelId("scene-panel"), "center-docs", new PanelId("scene-panel"));
        var sut = new SceneShellFocusCoordinator(shellState, "scene:1");

        sut.SetViewportFocus(true);
        Assert.Equal("scene:1", shellState.Focus.FocusedViewportId);
        Assert.Equal("scene:1", shellState.Focus.SelectionOwner);
        Assert.Equal("scene:1", shellState.Focus.KeyboardInputOwner);

        sut.SetViewportFocus(false);
        Assert.Null(shellState.Focus.FocusedViewportId);
        Assert.Null(shellState.Focus.SelectionOwner);
        Assert.Null(shellState.Focus.KeyboardInputOwner);
    }

    sealed class FakeSceneViewportBackend : ISceneViewportBackend
    {
        public ulong BoundViewportId { get; private set; }
        public nint BoundHostHandle { get; private set; }
        public int BindCount { get; private set; }
        public SceneViewportRect LastEditorViewport { get; private set; }
        public SceneViewportRenderTarget LastRenderTarget { get; private set; }
        public int RenderTargetSetCount { get; private set; }
        public List<ulong> UpdatedViewportIds { get; } = [];

        public void BindMacHost(ulong viewportId, nint hostHandle)
        {
            BoundViewportId = viewportId;
            BoundHostHandle = hostHandle;
            BindCount++;
        }

        public bool TryUpdateViewport(ulong viewportId, SceneViewportRect rect, bool visible, bool focused)
        {
            UpdatedViewportIds.Add(viewportId);
            return true;
        }

        public void SetEditorViewport(SceneViewportRect rect) => LastEditorViewport = rect;

        public void SetRenderTargetSize(uint width, uint height)
        {
            LastRenderTarget = new SceneViewportRenderTarget(width, height);
            RenderTargetSetCount++;
        }

        public void DestroyViewport(ulong viewportId) { }
        public void RetryViewport(ulong viewportId) { }
        public RemoteViewportSessionState GetViewportState(ulong viewportId) => RemoteViewportSessionState.Active;
        public string GetViewportDiagnostics(ulong viewportId) => string.Empty;
        public bool SendInput(ulong viewportId, RemoteViewportInputKind kind, float pointerX = 0, float pointerY = 0, float wheelDeltaX = 0, float wheelDeltaY = 0, uint keyCode = 0, uint button = 0, RemoteViewportInputModifier modifiers = RemoteViewportInputModifier.None, bool pressed = false, bool focused = false, bool captured = false) => true;
    }
}
