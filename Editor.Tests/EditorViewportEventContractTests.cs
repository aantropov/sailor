using SailorEditor.Scene;
using SailorEditor.Protocol.Generated;

namespace Editor.Tests;

public sealed class EditorViewportEventContractTests
{
    [Fact]
    public void SelectionEvent_ParsesSelectedAndClearedSelection()
    {
        Assert.True(EditorViewportEventContract.TryParse(
            "kind: selection\nrevision: 17\nmanagedMutationRevision: 5\nselectedInstanceId: go-42\n",
            out var selected,
            out var selectedError), selectedError);
        var selection = Assert.IsType<EditorViewportSelectionEvent>(selected);
        Assert.Equal(17UL, selection.Revision);
        Assert.Equal(5UL, selection.ManagedMutationRevision);
        Assert.Equal("go-42", selection.SelectedInstanceId);

        Assert.True(EditorViewportEventContract.TryParse(
            "kind: selection\nrevision: 18\nmanagedMutationRevision: 5\nselectedInstanceId: ''\n",
            out var cleared,
            out var clearedError), clearedError);
        Assert.Equal(string.Empty, Assert.IsType<EditorViewportSelectionEvent>(cleared).SelectedInstanceId);
    }

    [Fact]
    public void TransformEvent_ParsesTypedOperationSpaceAndNumericTransform()
    {
        const string yaml = """
            kind: transform
            revision: 21
            managedMutationRevision: 8
            instanceId: go-42
            operation: Rotate
            space: Local
            beforePosition: [0, 0, 0, 1]
            beforeRotation: [0, 0, 0, 1]
            beforeScale: [1, 1, 1, 0]
            afterPosition: [1, 2.5, -3, 1]
            afterRotation: [0, 0.70710677, 0, 0.70710677]
            afterScale: [1, 2, 3, 0]
            """;

        Assert.True(EditorViewportEventContract.TryParse(yaml, out var parsed, out var error), error);
        var transform = Assert.IsType<EditorViewportTransformEvent>(parsed);
        Assert.Equal(21UL, transform.Revision);
        Assert.Equal(8UL, transform.ManagedMutationRevision);
        Assert.Equal("go-42", transform.InstanceId);
        Assert.Equal(EditorViewportTransformOperation.Rotate, transform.Operation);
        Assert.Equal(EditorViewportTransformSpace.Local, transform.Space);
        Assert.Equal(new EditorViewportVector4(0, 0, 0, 1), transform.BeforePosition);
        Assert.Equal(new EditorViewportVector4(1, 2.5f, -3, 1), transform.AfterPosition);
        Assert.Equal(new EditorViewportVector4(1, 2, 3, 0), transform.AfterScale);
    }

    [Fact]
    public void TypedTransformEvent_MapsGeneratedProtocolPayload()
    {
        var source = new ViewportEvent
        {
            Revision = 21,
            ManagedMutationRevision = 8,
            Transform = new ViewportTransformEvent
            {
                InstanceId = "go-42",
                Operation = ViewportTransformOperation.Rotate,
                Space = ViewportTransformSpace.Local,
                BeforePosition = new Vector4 { W = 1 },
                BeforeRotation = new Vector4 { W = 1 },
                BeforeScale = new Vector4 { X = 1, Y = 1, Z = 1 },
                AfterPosition = new Vector4 { X = 1, Y = 2.5f, Z = -3, W = 1 },
                AfterRotation = new Vector4 { Y = 0.70710677f, W = 0.70710677f },
                AfterScale = new Vector4 { X = 1, Y = 2, Z = 3 }
            }
        };

        Assert.True(
            EditorViewportEventContract.TryCreate(
                source,
                out var viewportEvent,
                out var error),
            error);
        var transform = Assert.IsType<EditorViewportTransformEvent>(viewportEvent);
        Assert.Equal(21UL, transform.Revision);
        Assert.Equal(8UL, transform.ManagedMutationRevision);
        Assert.Equal(EditorViewportTransformOperation.Rotate, transform.Operation);
        Assert.Equal(EditorViewportTransformSpace.Local, transform.Space);
        Assert.Equal(new EditorViewportVector4(1, 2.5f, -3, 1), transform.AfterPosition);
    }

    [Fact]
    public void TypedTransformEvent_RejectsUnspecifiedEnumsAndNonFiniteVectors()
    {
        var source = new ViewportEvent
        {
            Transform = new ViewportTransformEvent
            {
                InstanceId = "go-42",
                Operation = ViewportTransformOperation.Unspecified,
                Space = ViewportTransformSpace.World
            }
        };

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var unsupported,
                out var unsupportedError));
        Assert.Null(unsupported);
        Assert.Contains("unsupported", unsupportedError, StringComparison.OrdinalIgnoreCase);

        source.Transform.Operation = ViewportTransformOperation.Translate;
        source.Transform.BeforePosition = new Vector4 { X = float.PositiveInfinity };
        source.Transform.BeforeRotation = new Vector4();
        source.Transform.BeforeScale = new Vector4();
        source.Transform.AfterPosition = new Vector4();
        source.Transform.AfterRotation = new Vector4();
        source.Transform.AfterScale = new Vector4();

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var nonFinite,
                out var nonFiniteError));
        Assert.Null(nonFinite);
        Assert.Contains("non-finite", nonFiniteError, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AssetDropEvent_MapsTypedAndYamlPayloads()
    {
        var source = new ViewportEvent
        {
            Revision = 31,
            ManagedMutationRevision = 9,
            AssetDrop = new ViewportAssetDropEvent
            {
                FileId = "{12345678-1234-1234-1234-123456789ABC}",
                NormalizedX = 0.25f,
                NormalizedY = 0.75f
            }
        };

        Assert.True(
            EditorViewportEventContract.TryCreate(
                source,
                out var typedEvent,
                out var typedError),
            typedError);
        var typedDrop =
            Assert.IsType<EditorViewportAssetDropEvent>(typedEvent);
        Assert.Equal(31ul, typedDrop.Revision);
        Assert.Equal(9ul, typedDrop.ManagedMutationRevision);
        Assert.Equal(source.AssetDrop.FileId, typedDrop.FileId);
        Assert.Equal(0.25f, typedDrop.NormalizedX);
        Assert.Equal(0.75f, typedDrop.NormalizedY);

        Assert.True(
            EditorViewportEventContract.TryParse(
                """
                kind: assetDrop
                revision: 32
                managedMutationRevision: 10
                fileId: "{12345678-1234-1234-1234-123456789ABC}"
                normalizedX: 0
                normalizedY: 1
                """,
                out var yamlEvent,
                out var yamlError),
            yamlError);
        var yamlDrop =
            Assert.IsType<EditorViewportAssetDropEvent>(yamlEvent);
        Assert.Equal(0f, yamlDrop.NormalizedX);
        Assert.Equal(1f, yamlDrop.NormalizedY);
    }

    [Fact]
    public void AssetDropEvent_RejectsMissingIdentityAndInvalidCoordinates()
    {
        var source = new ViewportEvent
        {
            AssetDrop = new ViewportAssetDropEvent
            {
                FileId = string.Empty,
                NormalizedX = 0.5f,
                NormalizedY = 0.5f
            }
        };

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var missingId,
                out var missingIdError));
        Assert.Null(missingId);
        Assert.Contains(
            "file",
            missingIdError,
            StringComparison.OrdinalIgnoreCase);

        source.AssetDrop.FileId =
            "{12345678-1234-1234-1234-123456789ABC}";
        source.AssetDrop.NormalizedX = float.NaN;
        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var nonFinite,
                out var nonFiniteError));
        Assert.Null(nonFinite);
        Assert.Contains(
            "normalized",
            nonFiniteError,
            StringComparison.OrdinalIgnoreCase);

        Assert.False(
            EditorViewportEventContract.TryParse(
                """
                kind: assetDrop
                revision: 33
                managedMutationRevision: 10
                fileId: "{12345678-1234-1234-1234-123456789ABC}"
                normalizedX: -0.1
                normalizedY: 0.5
                """,
                out var outOfRange,
                out var outOfRangeError));
        Assert.Null(outOfRange);
        Assert.Contains(
            "normalized",
            outOfRangeError,
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ToolShortcutEvent_MapsTypedAndYamlPayloads()
    {
        var source = new ViewportEvent
        {
            Revision = 34,
            ManagedMutationRevision = 12,
            ToolShortcut = new ViewportToolShortcutEvent
            {
                KeyCode = 'W'
            }
        };

        Assert.True(
            EditorViewportEventContract.TryCreate(
                source,
                out var typedEvent,
                out var typedError),
            typedError);
        var typedShortcut =
            Assert.IsType<EditorViewportToolShortcutEvent>(typedEvent);
        Assert.Equal(34ul, typedShortcut.Revision);
        Assert.Equal(12ul, typedShortcut.ManagedMutationRevision);
        Assert.Equal((uint)'W', typedShortcut.KeyCode);

        Assert.True(
            EditorViewportEventContract.TryParse(
                """
                kind: toolShortcut
                revision: 35
                managedMutationRevision: 13
                keyCode: 84
                """,
                out var yamlEvent,
                out var yamlError),
            yamlError);
        var yamlShortcut =
            Assert.IsType<EditorViewportToolShortcutEvent>(yamlEvent);
        Assert.Equal((uint)'T', yamlShortcut.KeyCode);
    }

    [Fact]
    public void ToolShortcutEvent_RejectsUnsupportedKeys()
    {
        var source = new ViewportEvent
        {
            ToolShortcut = new ViewportToolShortcutEvent
            {
                KeyCode = 'X'
            }
        };

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var typedEvent,
                out var typedError));
        Assert.Null(typedEvent);
        Assert.Contains(
            "unsupported",
            typedError,
            StringComparison.OrdinalIgnoreCase);

        Assert.False(
            EditorViewportEventContract.TryParse(
                """
                kind: toolShortcut
                revision: 36
                managedMutationRevision: 13
                keyCode: 119
                """,
                out var yamlEvent,
                out var yamlError));
        Assert.Null(yamlEvent);
        Assert.Contains(
            "unsupported",
            yamlError,
            StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("kind: unknown\nrevision: 1\nmanagedMutationRevision: 0\n", "Unsupported viewport event kind")]
    [InlineData("kind: selection\nrevision: 1\nmanagedMutationRevision: 0\nselectedInstanceId: go\nunexpected: true\n", "unexpected field")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: ''\noperation: Translate\nspace: World\nbeforePosition: [0, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "instanceId")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: translate\nspace: World\nbeforePosition: [0, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "unsupported value")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: Scale\nspace: World\nbeforePosition: [0, 0, 0]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "exactly four")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: Scale\nspace: World\nbeforePosition: [.inf, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "invalid number")]
    public void InvalidPayload_IsRejected(string yaml, string expectedError)
    {
        Assert.False(EditorViewportEventContract.TryParse(yaml, out var parsed, out var error));
        Assert.Null(parsed);
        Assert.Contains(expectedError, error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RevisionGate_RejectsDuplicateAndStaleEventsAcrossKinds()
    {
        var gate = new EditorViewportEventRevisionGate();
        var revisionSeven =
            new EditorViewportSelectionEvent(7, 0, "go-1");
        var revisionEight = Transform(8);

        Assert.True(gate.TryAccept(revisionSeven));
        Assert.True(gate.IsCurrent(revisionSeven));
        Assert.False(gate.TryAccept(Transform(7)));
        Assert.False(gate.TryAccept(Transform(6)));
        Assert.True(gate.TryAccept(revisionEight));
        Assert.False(gate.IsCurrent(revisionSeven));
        Assert.True(gate.IsCurrent(revisionEight));
        Assert.Equal(8UL, gate.LastAcceptedRevision);

        gate.Reset();
        Assert.Null(gate.LastAcceptedRevision);
        Assert.False(gate.IsCurrent(revisionEight));
        Assert.True(gate.TryAccept(new EditorViewportSelectionEvent(0, 0, string.Empty)));
    }

    [Fact]
    public void ManagedMutationOrder_RejectsDelayedViewportEventAfterNewerManagedEdit()
    {
        const ulong eventManagedMutationRevision = 12;

        Assert.True(EditorViewportMutationOrder.IsCurrent(eventManagedMutationRevision, 12));
        Assert.False(EditorViewportMutationOrder.IsCurrent(eventManagedMutationRevision, 13));
    }

    [Fact]
    public void EpochGate_RejectsBatchesCapturedBeforeDocumentChange()
    {
        var gate = new EditorViewportEventEpochGate();
        var previousDocument = gate.Current;

        Assert.True(gate.IsCurrent(previousDocument));
        Assert.Equal(previousDocument + 1, gate.Advance());
        Assert.False(gate.IsCurrent(previousDocument));
        Assert.True(gate.IsCurrent(gate.Current));
    }

    static EditorViewportTransformEvent Transform(ulong revision) => new(
        revision,
        0,
        "go-1",
        EditorViewportTransformOperation.Translate,
        EditorViewportTransformSpace.World,
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(1, 1, 1, 0),
        new EditorViewportVector4(1, 0, 0, 1),
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(1, 1, 1, 0));
}
