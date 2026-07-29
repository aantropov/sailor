namespace SailorEditor.Scene;

public readonly record struct SceneViewportToolState(
    EditorViewportTransformOperation Operation,
    EditorViewportTransformSpace Space);

public static class SceneViewportToolShortcuts
{
    public static SceneViewportToolState Default { get; } = new(
        EditorViewportTransformOperation.Select,
        EditorViewportTransformSpace.World);

    public static bool TryApply(
        uint keyCode,
        bool isPressed,
        bool hasViewportFocus,
        SceneViewportToolState current,
        out SceneViewportToolState next)
    {
        next = current;
        if (!isPressed || !hasViewportFocus)
        {
            return false;
        }

        var normalizedKeyCode = keyCode is >= 'a' and <= 'z'
            ? keyCode - ('a' - 'A')
            : keyCode;
        next = normalizedKeyCode switch
        {
            'Q' => current with
            {
                Operation = EditorViewportTransformOperation.Select
            },
            'W' => current with
            {
                Operation = EditorViewportTransformOperation.Translate
            },
            'E' => current with
            {
                Operation = EditorViewportTransformOperation.Rotate
            },
            'R' => current with
            {
                Operation = EditorViewportTransformOperation.Scale
            },
            'T' => current with
            {
                Space = current.Space == EditorViewportTransformSpace.Local
                    ? EditorViewportTransformSpace.World
                    : EditorViewportTransformSpace.Local
            },
            _ => current
        };

        return normalizedKeyCode is 'Q' or 'W' or 'E' or 'R' or 'T';
    }
}
