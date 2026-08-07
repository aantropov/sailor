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
            (nint)0xCAFE,
            1.5));

        Assert.True(updated);
        Assert.Equal([(ulong)7], backend.UpdatedViewportIds);
        Assert.Equal((ulong)7, backend.BoundViewportId);
        Assert.Equal((nint)0xCAFE, backend.BoundHostHandle);
        Assert.Equal(1.5, backend.BoundHostScale);
        Assert.Equal(new SceneViewportRect(0, 0, 800, 600), backend.LastEditorViewport);
        Assert.Equal(new SceneViewportRenderTarget(1600, 1200), backend.LastRenderTarget);
    }

    [Fact]
    public void Sync_ReannouncesStableHostBeforeUpdate_ButDoesNotReapplyStableRenderTarget()
    {
        var backend = new FakeSceneViewportBackend();
        var sut = new SceneViewportLifecycleAdapter(backend, 7);
        var frame = new SceneViewportFrame(new SceneViewportRect(0, 0, 640, 480), new SceneViewportRect(0, 0, 640, 480), new SceneViewportRenderTarget(640, 480), true, false, (nint)42);

        sut.Sync(frame);
        sut.Sync(frame);

        Assert.Equal(2, backend.BindCount);
        Assert.Equal(1, backend.RenderTargetSetCount);
        Assert.Equal(2, backend.UpdatedViewportIds.Count);
        Assert.Equal(["bind:42", "update", "bind:42", "update"], backend.Operations);
    }

    [Fact]
    public void BackendRestart_RebindsStableHostAndRenderTarget()
    {
        var backend = new FakeSceneViewportBackend();
        var sut = new SceneViewportLifecycleAdapter(backend, 7);
        var frame = new SceneViewportFrame(
            new SceneViewportRect(0, 0, 640, 480),
            new SceneViewportRect(0, 0, 640, 480),
            new SceneViewportRenderTarget(1280, 960),
            true,
            false,
            (nint)42);

        sut.Sync(frame);
        sut.ResetForBackendRestart();
        sut.Sync(frame);

        Assert.Equal(2, backend.BindCount);
        Assert.Equal(2, backend.RenderTargetSetCount);
        Assert.Equal(2, backend.UpdatedViewportIds.Count);
    }

    [Fact]
    public void Sync_ClearsObservedHost_WhenFrameNoLongerProvidesOne()
    {
        var backend = new FakeSceneViewportBackend();
        var sut = new SceneViewportLifecycleAdapter(backend, 7);

        sut.Sync(new SceneViewportFrame(
            new SceneViewportRect(0, 0, 640, 480),
            new SceneViewportRect(0, 0, 640, 480),
            default,
            true,
            false,
            (nint)42));
        sut.Sync(new SceneViewportFrame(
            new SceneViewportRect(0, 0, 640, 480),
            new SceneViewportRect(0, 0, 640, 480),
            default,
            true,
            false));

        Assert.Equal([(nint)42, nint.Zero], backend.BoundHostHandles);
    }

    [Fact]
    public void Destroy_UnbindsObservedHostBeforeDestroyingViewport_AndRemainsIdempotent()
    {
        var backend = new FakeSceneViewportBackend();
        var sut = new SceneViewportLifecycleAdapter(backend, 7);

        sut.Sync(new SceneViewportFrame(
            new SceneViewportRect(0, 0, 640, 480),
            new SceneViewportRect(0, 0, 640, 480),
            default,
            true,
            false,
            (nint)42));

        sut.Destroy();
        sut.Destroy();

        Assert.Equal([(nint)42, nint.Zero], backend.BoundHostHandles);
        Assert.Equal(1, backend.DestroyCount);
        Assert.Equal(["bind:42", "update", "bind:0", "destroy"], backend.Operations);
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

    [Fact]
    public void FocusCoordinator_PromotesScenePanelToActiveDocumentWhenViewportClaimsFocus()
    {
        var shellState = new ShellState();
        shellState.FocusPanel(new PanelId("console"), "bottom-console", activeDocument: null);
        var scenePanelId = new PanelId("scene-panel");
        var sut = new SceneShellFocusCoordinator(
            shellState,
            "scene:1",
            () => new SceneShellFocusTarget(scenePanelId, "center-docs", scenePanelId));

        sut.SetViewportFocus(true);

        Assert.Equal(scenePanelId, shellState.Focus.FocusedPanelId);
        Assert.Equal("center-docs", shellState.Focus.ActiveTabGroupId);
        Assert.Equal(scenePanelId, shellState.Focus.ActiveDocumentPanelId);
        Assert.Equal("scene:1", shellState.Focus.FocusedViewportId);
    }

    [Fact]
    public void PointerRouting_RejectsGlobalPressOutsideViewport()
    {
        Assert.False(SceneViewportPointerRouting.ShouldAcceptMouseButton(
            pressed: true,
            hasLocalHit: false,
            hasPointerSample: true,
            SailorEditor.Controls.NativeSceneViewportInputModifier.None,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseLeft));
    }

    [Fact]
    public void PointerRouting_AcceptsInsidePressAndCapturedOutsideRelease()
    {
        Assert.True(SceneViewportPointerRouting.ShouldAcceptMouseButton(
            pressed: true,
            hasLocalHit: true,
            hasPointerSample: true,
            SailorEditor.Controls.NativeSceneViewportInputModifier.None,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseLeft));

        Assert.True(SceneViewportPointerRouting.ShouldAcceptMouseButton(
            pressed: false,
            hasLocalHit: false,
            hasPointerSample: true,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseLeft,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseLeft));
    }

    [Fact]
    public void PointerRouting_AcceptsAdditionalButtonWhileViewportOwnsCapture()
    {
        Assert.True(SceneViewportPointerRouting.ShouldAcceptMouseButton(
            pressed: true,
            hasLocalHit: false,
            hasPointerSample: true,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseLeft,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseRight));
    }

    [Fact]
    public void PointerRouting_RejectsReleaseForButtonViewportDidNotCapture()
    {
        Assert.False(SceneViewportPointerRouting.ShouldAcceptMouseButton(
            pressed: false,
            hasLocalHit: true,
            hasPointerSample: true,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseRight,
            SailorEditor.Controls.NativeSceneViewportInputModifier.MouseLeft));
    }

    [Fact]
    public void PointerRouting_UsesCapturedMotionOnlyWhileViewportOwnsCapture()
    {
        var rightCaptured = SailorEditor.Controls.NativeSceneViewportInputModifier.MouseRight;

        Assert.False(SceneViewportPointerRouting.ShouldPublishHoverMove(rightCaptured));
        Assert.True(SceneViewportPointerRouting.ShouldPublishCapturedMove(
            hasPointerSample: true,
            rightCaptured));
        Assert.False(SceneViewportPointerRouting.ShouldPublishCapturedMove(
            hasPointerSample: false,
            rightCaptured));
        Assert.True(SceneViewportPointerRouting.ShouldPublishHoverMove(
            SailorEditor.Controls.NativeSceneViewportInputModifier.None));
    }

    [Theory]
    [InlineData(RemoteViewportSessionState.Active, "Remote viewport active")]
    [InlineData(RemoteViewportSessionState.Ready, "Remote viewport ready")]
    [InlineData(RemoteViewportSessionState.Recovering, "Remote viewport reconnecting…")]
    [InlineData(RemoteViewportSessionState.Lost, "Remote viewport unavailable — retrying session")]
    [InlineData(RemoteViewportSessionState.Created, "Remote viewport unavailable")]
    public void StatusText_DescribesViewportStateWithoutLegacyFallback(RemoteViewportSessionState state, string expected)
    {
        Assert.Equal(expected, SceneViewportStatusText.Describe(state));
        Assert.DoesNotContain("legacy", SceneViewportStatusText.Describe(state), StringComparison.OrdinalIgnoreCase);
    }

    sealed class FakeSceneViewportBackend : ISceneViewportBackend
    {
        public ulong BoundViewportId { get; private set; }
        public nint BoundHostHandle { get; private set; }
        public double BoundHostScale { get; private set; }
        public int BindCount { get; private set; }
        public List<nint> BoundHostHandles { get; } = [];
        public SceneViewportRect LastEditorViewport { get; private set; }
        public SceneViewportRenderTarget LastRenderTarget { get; private set; }
        public int RenderTargetSetCount { get; private set; }
        public int DestroyCount { get; private set; }
        public List<ulong> UpdatedViewportIds { get; } = [];
        public List<string> Operations { get; } = [];

        public void BindMacHost(ulong viewportId, nint hostHandle, double compositionScale)
        {
            BoundViewportId = viewportId;
            BoundHostHandle = hostHandle;
            BoundHostScale = compositionScale;
            BindCount++;
            BoundHostHandles.Add(hostHandle);
            Operations.Add($"bind:{hostHandle}");
        }

        public bool TryUpdateViewport(ulong viewportId, SceneViewportRect rect, bool visible, bool focused)
        {
            UpdatedViewportIds.Add(viewportId);
            Operations.Add("update");
            return true;
        }

        public void SetEditorViewport(SceneViewportRect rect) => LastEditorViewport = rect;

        public void SetRenderTargetSize(uint width, uint height)
        {
            LastRenderTarget = new SceneViewportRenderTarget(width, height);
            RenderTargetSetCount++;
        }

        public void DestroyViewport(ulong viewportId)
        {
            DestroyCount++;
            Operations.Add("destroy");
        }
        public void RetryViewport(ulong viewportId) { }
        public RemoteViewportSessionState GetViewportState(ulong viewportId) => RemoteViewportSessionState.Active;
        public string GetViewportDiagnostics(ulong viewportId) => string.Empty;
        public bool SendInput(ulong viewportId, RemoteViewportInputKind kind, float pointerX = 0, float pointerY = 0, float wheelDeltaX = 0, float wheelDeltaY = 0, uint keyCode = 0, uint button = 0, RemoteViewportInputModifier modifiers = RemoteViewportInputModifier.None, bool pressed = false, bool focused = false, bool captured = false) => true;
    }
}
