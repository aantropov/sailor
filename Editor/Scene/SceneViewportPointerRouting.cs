using SailorEditor.Controls;

namespace SailorEditor.Scene;

public static class SceneViewportPointerRouting
{
    public static bool ShouldAcceptMouseButton(
        bool pressed,
        bool hasLocalHit,
        bool hasPointerSample,
        NativeSceneViewportInputModifier activeButtons,
        NativeSceneViewportInputModifier button)
    {
        if (button is not NativeSceneViewportInputModifier.MouseLeft and
            not NativeSceneViewportInputModifier.MouseRight and
            not NativeSceneViewportInputModifier.MouseMiddle)
        {
            return false;
        }

        if (pressed)
        {
            var hasPointerCapture = HasPointerCapture(activeButtons);
            return (hasLocalHit || hasPointerCapture) &&
                hasPointerSample &&
                (activeButtons & button) == 0;
        }

        return (activeButtons & button) != 0;
    }

    public static bool ShouldPublishHoverMove(NativeSceneViewportInputModifier activeButtons)
        => !HasPointerCapture(activeButtons);

    public static bool ShouldPublishCapturedMove(
        bool hasPointerSample,
        NativeSceneViewportInputModifier activeButtons)
        => hasPointerSample && HasPointerCapture(activeButtons);

    static bool HasPointerCapture(NativeSceneViewportInputModifier modifiers)
    {
        const NativeSceneViewportInputModifier mouseButtons =
            NativeSceneViewportInputModifier.MouseLeft |
            NativeSceneViewportInputModifier.MouseRight |
            NativeSceneViewportInputModifier.MouseMiddle;
        return (modifiers & mouseButtons) != 0;
    }
}
