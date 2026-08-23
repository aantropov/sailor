using SailorEditor.Scene;

namespace Editor.Tests;

public sealed class SceneViewportInteractionTests
{
    [Theory]
    [InlineData("lit", SceneViewRenderMode.Lit)]
    [InlineData("ambient_occlusion", SceneViewRenderMode.AmbientOcclusion)]
    [InlineData("Ambient Occlusion", SceneViewRenderMode.AmbientOcclusion)]
    [InlineData("AO", SceneViewRenderMode.AmbientOcclusion)]
    [InlineData("csm", SceneViewRenderMode.Cascades)]
    [InlineData("light-tiles", SceneViewRenderMode.LightTiles)]
    public void RenderModeName_ParsesMcpValues(
        string value,
        SceneViewRenderMode expected)
    {
        Assert.True(SceneViewRenderModeNames.TryParse(value, out var mode));
        Assert.Equal(expected, mode);
        Assert.Contains(
            SceneViewRenderModeNames.ToExternalName(mode),
            SceneViewRenderModeNames.Supported);
    }

    [Theory]
    [InlineData("")]
    [InlineData("depth")]
    [InlineData("ambient")]
    public void RenderModeName_RejectsUnsupportedValues(string value)
    {
        Assert.False(SceneViewRenderModeNames.TryParse(value, out _));
    }

    [Theory]
    [InlineData('Q', EditorViewportTransformOperation.Select)]
    [InlineData('W', EditorViewportTransformOperation.Translate)]
    [InlineData('E', EditorViewportTransformOperation.Rotate)]
    [InlineData('R', EditorViewportTransformOperation.Scale)]
    [InlineData('w', EditorViewportTransformOperation.Translate)]
    public void ToolShortcut_ChangesOperationOnlyWithViewportFocus(
        char shortcut,
        EditorViewportTransformOperation expectedOperation)
    {
        var current = new SceneViewportToolState(
            EditorViewportTransformOperation.Scale,
            EditorViewportTransformSpace.World);

        Assert.True(SceneViewportToolShortcuts.TryApply(
            shortcut,
            isPressed: true,
            hasViewportFocus: true,
            current,
            out var next));
        Assert.Equal(expectedOperation, next.Operation);
        Assert.Equal(current.Space, next.Space);
        Assert.False(SceneViewportToolShortcuts.TryApply(
            shortcut,
            isPressed: true,
            hasViewportFocus: false,
            current,
            out var ignored));
        Assert.Equal(current, ignored);
    }

    [Fact]
    public void ToolShortcut_TogglesTransformSpace()
    {
        var world = new SceneViewportToolState(
            EditorViewportTransformOperation.Translate,
            EditorViewportTransformSpace.World);

        Assert.True(SceneViewportToolShortcuts.TryApply(
            'T',
            isPressed: true,
            hasViewportFocus: true,
            world,
            out var local));
        Assert.Equal(EditorViewportTransformSpace.Local, local.Space);
        Assert.True(SceneViewportToolShortcuts.TryApply(
            'T',
            isPressed: true,
            hasViewportFocus: true,
            local,
            out var restored));
        Assert.Equal(EditorViewportTransformSpace.World, restored.Space);
    }

    [Fact]
    public void ToolShortcut_IgnoresReleaseAndUnknownKey()
    {
        var current = SceneViewportToolShortcuts.Default;

        Assert.False(SceneViewportToolShortcuts.TryApply(
            'W',
            isPressed: false,
            hasViewportFocus: true,
            current,
            out _));
        Assert.False(SceneViewportToolShortcuts.TryApply(
            'X',
            isPressed: true,
            hasViewportFocus: true,
            current,
            out _));
    }

    [Theory]
    [InlineData(50, 25, 100, 50, 0.5, 0.5)]
    [InlineData(-10, 75, 100, 50, 0.0, 1.0)]
    [InlineData(125, -5, 100, 50, 1.0, 0.0)]
    public void DropCoordinates_NormalizeAndClamp(
        double x,
        double y,
        double width,
        double height,
        double expectedX,
        double expectedY)
    {
        Assert.True(SceneViewportDropCoordinateResolver.TryResolve(
            x,
            y,
            width,
            height,
            out var coordinates));
        Assert.Equal(expectedX, coordinates.NormalizedX);
        Assert.Equal(expectedY, coordinates.NormalizedY);
    }

    [Fact]
    public void DropCoordinates_RejectInvalidViewport()
    {
        Assert.False(SceneViewportDropCoordinateResolver.TryResolve(
            10,
            10,
            0,
            100,
            out _));
        Assert.False(SceneViewportDropCoordinateResolver.TryResolve(
            double.NaN,
            10,
            100,
            100,
            out _));
    }

    [Theory]
    [InlineData("{01234567-89AB-CDEF-0123-456789ABCDEF}")]
    [InlineData("{01234567-89ab-cdef-0123-456789abcdef}")]
    [InlineData("01234567-89AB-CDEF-0123-456789ABCDEF")]
    [InlineData("01234567-89ab-cdef-0123-456789abcdef")]
    public void AssetDropPayload_CreatesForEverySerializedFileIdFormat(
        string fileId)
    {
        Assert.True(SceneViewportAssetDropPayload.TryCreate(
            new SailorEngine.FileId(fileId),
            out var payload));
        Assert.Equal(
            SceneViewportAssetDropPayload.Prefix + fileId,
            payload);
        Assert.Equal(
            SceneViewportAssetDropPayload.Prefix.Length + fileId.Length,
            payload.Length);
        Assert.True(
            payload.Length <= SceneViewportAssetDropPayload.MaxLength);
    }

    [Theory]
    [InlineData("0123456789AB-CDEF-0123-456789ABCDEF")]
    [InlineData("01234567-89AB-CDEG-0123-456789ABCDEF")]
    [InlineData("01234567-89AB-CDEF-0123-456789ABCDEFsuffix")]
    [InlineData("{0123456789AB-CDEF-0123-456789ABCDEF}")]
    [InlineData("{01234567-89AB-CDEG-0123-456789ABCDEF}")]
    [InlineData("{01234567-89AB-CDEF-0123-456789ABCDEF}suffix")]
    [InlineData("[01234567-89AB-CDEF-0123-456789ABCDEF]")]
    [InlineData("{01234567-89AB-CDEF-0123-456789ABCDEF")]
    [InlineData("")]
    public void AssetDropPayload_RejectsMalformedOrOversizedFileId(
        string fileId)
    {
        Assert.False(SceneViewportAssetDropPayload.TryCreate(
            new SailorEngine.FileId(fileId),
            out var payload));
        Assert.Equal(string.Empty, payload);
    }
}
