namespace SailorEditor.Scene;

public enum SceneViewRenderMode
{
    Lit = 0,
    AmbientOcclusion,
    Cascades,
    LightTiles,
    GlobalIlluminationOnly,
    GlobalIlluminationProbes,
    GlobalIlluminationBricks,
    GlobalIlluminationValidity,
    GlobalIlluminationVisibility,
    GlobalIlluminationResidency,
    GlobalIlluminationAssetIdentity,
    GlobalIlluminationFallback,
    GlobalIlluminationClipmapCascades
}

public static class SceneViewRenderModeNames
{
    public static IReadOnlyList<string> Supported { get; } =
    [
        "lit",
        "ambient_occlusion",
        "cascades",
        "light_tiles",
        "global_illumination_only",
        "global_illumination_probes",
        "global_illumination_bricks",
        "global_illumination_validity",
        "global_illumination_visibility",
        "global_illumination_residency",
        "global_illumination_asset_identity",
        "global_illumination_fallback",
        "global_illumination_clipmap_cascades"
    ];

    public static string ToExternalName(SceneViewRenderMode mode) => mode switch
    {
        SceneViewRenderMode.Lit => "lit",
        SceneViewRenderMode.AmbientOcclusion => "ambient_occlusion",
        SceneViewRenderMode.Cascades => "cascades",
        SceneViewRenderMode.LightTiles => "light_tiles",
        SceneViewRenderMode.GlobalIlluminationOnly =>
            "global_illumination_only",
        SceneViewRenderMode.GlobalIlluminationProbes =>
            "global_illumination_probes",
        SceneViewRenderMode.GlobalIlluminationBricks =>
            "global_illumination_bricks",
        SceneViewRenderMode.GlobalIlluminationValidity =>
            "global_illumination_validity",
        SceneViewRenderMode.GlobalIlluminationVisibility =>
            "global_illumination_visibility",
        SceneViewRenderMode.GlobalIlluminationResidency =>
            "global_illumination_residency",
        SceneViewRenderMode.GlobalIlluminationAssetIdentity =>
            "global_illumination_asset_identity",
        SceneViewRenderMode.GlobalIlluminationFallback =>
            "global_illumination_fallback",
        SceneViewRenderMode.GlobalIlluminationClipmapCascades =>
            "global_illumination_clipmap_cascades",
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
            "globalilluminationonly" or "gionly" =>
                SceneViewRenderMode.GlobalIlluminationOnly,
            "globalilluminationprobes" or "giprobes" or "probes" =>
                SceneViewRenderMode.GlobalIlluminationProbes,
            "globalilluminationbricks" or "gibricks" or "bricks" =>
                SceneViewRenderMode.GlobalIlluminationBricks,
            "globalilluminationvalidity" or "givalidity" =>
                SceneViewRenderMode.GlobalIlluminationValidity,
            "globalilluminationvisibility" or "givisibility" =>
                SceneViewRenderMode.GlobalIlluminationVisibility,
            "globalilluminationresidency" or "giresidency" =>
                SceneViewRenderMode.GlobalIlluminationResidency,
            "globalilluminationassetidentity" or "giassetidentity" =>
                SceneViewRenderMode.GlobalIlluminationAssetIdentity,
            "globalilluminationfallback" or "gifallback" =>
                SceneViewRenderMode.GlobalIlluminationFallback,
            "globalilluminationclipmapcascades" or "giclipmapcascades" or
                "gicascades" =>
                SceneViewRenderMode.GlobalIlluminationClipmapCascades,
            _ => default
        };
        return normalized is "lit" or "ambientocclusion" or "ao" or
            "cascades" or "csm" or "lighttiles" or
            "globalilluminationonly" or "gionly" or
            "globalilluminationprobes" or "giprobes" or "probes" or
            "globalilluminationbricks" or "gibricks" or "bricks" or
            "globalilluminationvalidity" or "givalidity" or
            "globalilluminationvisibility" or "givisibility" or
            "globalilluminationresidency" or "giresidency" or
            "globalilluminationassetidentity" or "giassetidentity" or
            "globalilluminationfallback" or "gifallback" or
            "globalilluminationclipmapcascades" or "giclipmapcascades" or
            "gicascades";
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
