using SailorEditor.Panels;
using SailorEditor.Services;
using SailorEditor.State;

namespace SailorEditor.Scene;

public readonly record struct SceneViewportRect(double X, double Y, double Width, double Height)
{
    public bool IsEmpty => Width <= 0 || Height <= 0;
}

public readonly record struct SceneViewportRenderTarget(uint Width, uint Height)
{
    public bool IsValid => Width > 0 && Height > 0;
}

public readonly record struct SceneViewportFrame(
    SceneViewportRect RemoteRect,
    SceneViewportRect EditorViewport,
    SceneViewportRenderTarget RenderTarget,
    bool IsVisible,
    bool IsFocused,
    nint NativeHostHandle = 0)
{
    public bool HasNativeHost => NativeHostHandle != 0;
}

public interface ISceneViewportBackend
{
    void BindMacHost(ulong viewportId, nint hostHandle);
    bool TryUpdateViewport(ulong viewportId, SceneViewportRect rect, bool visible, bool focused);
    void SetEditorViewport(SceneViewportRect rect);
    void SetRenderTargetSize(uint width, uint height);
    void DestroyViewport(ulong viewportId);
    void RetryViewport(ulong viewportId);
    RemoteViewportSessionState GetViewportState(ulong viewportId);
    string GetViewportDiagnostics(ulong viewportId);
    bool SendInput(
        ulong viewportId,
        RemoteViewportInputKind kind,
        float pointerX = 0,
        float pointerY = 0,
        float wheelDeltaX = 0,
        float wheelDeltaY = 0,
        uint keyCode = 0,
        uint button = 0,
        RemoteViewportInputModifier modifiers = RemoteViewportInputModifier.None,
        bool pressed = false,
        bool focused = false,
        bool captured = false);
}

public sealed class SceneViewportLifecycleAdapter(ISceneViewportBackend backend, ulong viewportId)
{
    nint _boundHostHandle;
    SceneViewportRenderTarget _renderTarget;
    bool _destroyed;

    public bool Sync(SceneViewportFrame frame)
    {
        _destroyed = false;

        if (frame.HasNativeHost && frame.NativeHostHandle != _boundHostHandle)
        {
            backend.BindMacHost(viewportId, frame.NativeHostHandle);
            _boundHostHandle = frame.NativeHostHandle;
        }

        if (!frame.EditorViewport.IsEmpty)
            backend.SetEditorViewport(frame.EditorViewport);

        if (frame.RenderTarget.IsValid && frame.RenderTarget != _renderTarget)
        {
            backend.SetRenderTargetSize(frame.RenderTarget.Width, frame.RenderTarget.Height);
            _renderTarget = frame.RenderTarget;
        }

        if (frame.RemoteRect.IsEmpty)
            return false;

        return backend.TryUpdateViewport(viewportId, frame.RemoteRect, frame.IsVisible, frame.IsFocused);
    }

    public void Retry() => backend.RetryViewport(viewportId);

    public void Destroy()
    {
        if (_destroyed)
            return;

        backend.DestroyViewport(viewportId);
        _destroyed = true;
        _boundHostHandle = 0;
        _renderTarget = default;
    }

    public RemoteViewportSessionState GetState() => backend.GetViewportState(viewportId);

    public string GetDiagnostics() => backend.GetViewportDiagnostics(viewportId);

    public bool SendInput(
        RemoteViewportInputKind kind,
        float pointerX = 0,
        float pointerY = 0,
        float wheelDeltaX = 0,
        float wheelDeltaY = 0,
        uint keyCode = 0,
        uint button = 0,
        RemoteViewportInputModifier modifiers = RemoteViewportInputModifier.None,
        bool pressed = false,
        bool focused = false,
        bool captured = false)
        => backend.SendInput(viewportId, kind, pointerX, pointerY, wheelDeltaX, wheelDeltaY, keyCode, button, modifiers, pressed, focused, captured);
}

public static class SceneViewportStatusText
{
    public static string Describe(RemoteViewportSessionState state) => state switch
    {
        RemoteViewportSessionState.Active => "Remote viewport active",
        RemoteViewportSessionState.Ready => "Remote viewport ready",
        RemoteViewportSessionState.Negotiating => "Connecting remote viewport…",
        RemoteViewportSessionState.Resizing => "Resizing remote viewport…",
        RemoteViewportSessionState.Recovering => "Remote viewport reconnecting…",
        RemoteViewportSessionState.Lost => "Remote viewport unavailable — retrying session",
        RemoteViewportSessionState.Paused => "Remote viewport paused",
        RemoteViewportSessionState.Terminating => "Remote viewport terminating…",
        RemoteViewportSessionState.Disposed => "Remote viewport disposed",
        _ => "Remote viewport unavailable"
    };
}

public readonly record struct SceneShellFocusTarget(
    PanelId? PanelId = null,
    string? GroupId = null,
    PanelId? ActiveDocumentPanelId = null);

public sealed class SceneShellFocusCoordinator(ShellState shellState, string viewportId, Func<SceneShellFocusTarget>? focusTargetProvider = null)
{
    public void SetViewportFocus(bool isFocused)
    {
        if (isFocused)
        {
            var focusTarget = focusTargetProvider?.Invoke();
            if (focusTarget?.PanelId is { } panelId)
            {
                shellState.FocusPanel(panelId, focusTarget.Value.GroupId, focusTarget.Value.ActiveDocumentPanelId ?? panelId);
            }

            shellState.FocusViewport(viewportId, selectionOwner: viewportId, keyboardInputOwner: viewportId);
            return;
        }

        if (shellState.Focus.FocusedViewportId == viewportId)
            shellState.FocusViewport(null, selectionOwner: null, keyboardInputOwner: null);
    }

    public void ReleaseIfOwned()
    {
        if (shellState.Focus.FocusedViewportId == viewportId)
            shellState.FocusViewport(null, selectionOwner: null, keyboardInputOwner: null);
    }
}
