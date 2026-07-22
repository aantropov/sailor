namespace SailorEditor.Services;

internal readonly record struct SceneViewportStateSnapshot<T>(
    T Rect,
    bool Visible,
    bool Focused);

internal sealed class LatestSceneViewportState<T>(
    T initialRect,
    bool initialVisible = true,
    bool initialFocused = false)
{
    SceneViewportStateSnapshot<T> latest = new(initialRect, initialVisible, initialFocused);

    public void Remember(T rect, bool visible, bool focused)
        => latest = new SceneViewportStateSnapshot<T>(rect, visible, focused);

    public void RememberRect(T rect)
        => latest = latest with { Rect = rect };

    public SceneViewportStateSnapshot<T> Capture() => latest;
}
