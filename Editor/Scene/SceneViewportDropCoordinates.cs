namespace SailorEditor.Scene;

public readonly record struct SceneViewportDropCoordinates(
    double NormalizedX,
    double NormalizedY);

public static class SceneViewportDropCoordinateResolver
{
    public static bool TryResolve(
        double pointerX,
        double pointerY,
        double viewportWidth,
        double viewportHeight,
        out SceneViewportDropCoordinates coordinates)
    {
        coordinates = default;
        if (!double.IsFinite(pointerX) ||
            !double.IsFinite(pointerY) ||
            !double.IsFinite(viewportWidth) ||
            !double.IsFinite(viewportHeight) ||
            viewportWidth <= 0 ||
            viewportHeight <= 0)
        {
            return false;
        }

        coordinates = new SceneViewportDropCoordinates(
            Math.Clamp(pointerX / viewportWidth, 0.0, 1.0),
            Math.Clamp(pointerY / viewportHeight, 0.0, 1.0));
        return true;
    }
}
