using SailorEditor.Services;

namespace SailorEditor.Scene;

internal sealed class EngineSceneViewportBackend(EngineService engineService) : ISceneViewportBackend
{
    public void BindMacHost(ulong viewportId, nint hostHandle) => engineService.BindMacRemoteViewportHost(viewportId, hostHandle);

    public bool TryUpdateViewport(ulong viewportId, SceneViewportRect rect, bool visible, bool focused)
        => engineService.TryUpdateRemoteViewport(viewportId, new Rect(rect.X, rect.Y, rect.Width, rect.Height), visible, focused);

    public void SetEditorViewport(SceneViewportRect rect) => engineService.Viewport = new Rect(rect.X, rect.Y, rect.Width, rect.Height);

    public void SetRenderTargetSize(uint width, uint height) => engineService.SetEditorRenderTargetSize(width, height);

    public void DestroyViewport(ulong viewportId) => engineService.DestroyRemoteViewport(viewportId);

    public void RetryViewport(ulong viewportId) => engineService.RetryRemoteViewport(viewportId);

    public RemoteViewportSessionState GetViewportState(ulong viewportId) => engineService.GetRemoteViewportState(viewportId);

    public string GetViewportDiagnostics(ulong viewportId) => engineService.GetRemoteViewportDiagnostics(viewportId);

    public bool SendInput(ulong viewportId, RemoteViewportInputKind kind, float pointerX = 0, float pointerY = 0, float wheelDeltaX = 0, float wheelDeltaY = 0, uint keyCode = 0, uint button = 0, RemoteViewportInputModifier modifiers = RemoteViewportInputModifier.None, bool pressed = false, bool focused = false, bool captured = false)
        => engineService.SendRemoteViewportInput(viewportId, kind, pointerX, pointerY, wheelDeltaX, wheelDeltaY, keyCode, button, modifiers, pressed, focused, captured);
}
