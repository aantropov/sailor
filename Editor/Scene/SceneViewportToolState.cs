namespace SailorEditor.Scene;

public enum SceneViewRenderMode
{
    Lit = 0,
    AmbientOcclusion,
    Cascades,
    LightTiles
}

public static class SceneViewRenderModeNames
{
    public static IReadOnlyList<string> Supported { get; } =
    [
        "lit",
        "ambient_occlusion",
        "cascades",
        "light_tiles"
    ];

    public static string ToExternalName(SceneViewRenderMode mode) => mode switch
    {
        SceneViewRenderMode.Lit => "lit",
        SceneViewRenderMode.AmbientOcclusion => "ambient_occlusion",
        SceneViewRenderMode.Cascades => "cascades",
        SceneViewRenderMode.LightTiles => "light_tiles",
        _ => throw new ArgumentOutOfRangeException(nameof(mode))
    };

    public static bool TryParse(
        string? value,
        out SceneViewRenderMode mode)
    {
        var normalized = value?
            .Trim()
            .Replace("-", string.Empty, StringComparison.Ordinal)
            .Replace("_", string.Empty, StringComparison.Ordinal)
            .Replace(" ", string.Empty, StringComparison.Ordinal)
            .ToLowerInvariant();
        mode = normalized switch
        {
            "lit" => SceneViewRenderMode.Lit,
            "ambientocclusion" or "ao" =>
                SceneViewRenderMode.AmbientOcclusion,
            "cascades" or "csm" => SceneViewRenderMode.Cascades,
            "lighttiles" => SceneViewRenderMode.LightTiles,
            _ => default
        };
        return normalized is "lit" or "ambientocclusion" or "ao" or
            "cascades" or "csm" or "lighttiles";
    }
}

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
