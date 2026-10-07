using Google.Protobuf;
using SailorEditor.Scene;
using SailorEditor.Protocol.Generated;

namespace Editor.Tests;

public sealed class EditorViewportEventContractTests
{
    [Theory]
    [InlineData("go-42")]
    [InlineData("")]
    [InlineData("船-42")]
    public void SelectionEvent_RoundTripsSelectionAndClear(string instanceId)
    {
        var selection = Assert.IsType<EditorViewportSelectionEvent>(Create(new ViewportEvent
        {
            Revision = 17,
            ManagedMutationRevision = 5,
            Selection = new ViewportSelectionEvent { SelectedInstanceId = instanceId }
        }));

        Assert.Equal(17UL, selection.Revision);
        Assert.Equal(5UL, selection.ManagedMutationRevision);
        Assert.Equal(instanceId, selection.SelectedInstanceId);
    }

    [Theory]
    [InlineData(ViewportTransformOperation.Select, EditorViewportTransformOperation.Select)]
    [InlineData(ViewportTransformOperation.Translate, EditorViewportTransformOperation.Translate)]
    [InlineData(ViewportTransformOperation.Rotate, EditorViewportTransformOperation.Rotate)]
    [InlineData(ViewportTransformOperation.Scale, EditorViewportTransformOperation.Scale)]
    public void TransformEvent_RoundTripsOperationSpaceAndBothPoses(
        ViewportTransformOperation operation,
        EditorViewportTransformOperation expectedOperation)
    {
        foreach (var (space, expectedSpace) in new[]
        {
            (ViewportTransformSpace.World, EditorViewportTransformSpace.World),
            (ViewportTransformSpace.Local, EditorViewportTransformSpace.Local)
        })
        {
            var source = TransformPayload();
            source.Transform.Operation = operation;
            source.Transform.Space = space;
            var transform = Assert.IsType<EditorViewportTransformEvent>(Create(source));

            Assert.Equal(21UL, transform.Revision);
            Assert.Equal(8UL, transform.ManagedMutationRevision);
            Assert.Equal("go-42", transform.InstanceId);
            Assert.Equal(expectedOperation, transform.Operation);
            Assert.Equal(expectedSpace, transform.Space);
            Assert.Equal(new EditorViewportVector4(0, 0, 0, 1), transform.BeforePosition);
            Assert.Equal(new EditorViewportVector4(0, 0, 0, 1), transform.BeforeRotation);
            Assert.Equal(new EditorViewportVector4(1, 1, 1, 0), transform.BeforeScale);
            Assert.Equal(new EditorViewportVector4(1, 2.5f, -3, 1), transform.AfterPosition);
            Assert.Equal(new EditorViewportVector4(0, 0.70710677f, 0, 0.70710677f), transform.AfterRotation);
            Assert.Equal(new EditorViewportVector4(1, 2, 3, 0), transform.AfterScale);
        }
    }

    [Fact]
    public void TransformEvent_DoesNotRetainMutableProtocolVectors()
    {
        var source = TransformPayload();
        Assert.True(EditorViewportEventContract.TryCreate(source, out var result, out var error), error);
        var transform = Assert.IsType<EditorViewportTransformEvent>(result);

        source.Transform.BeforePosition.X = 99;
        source.Transform.AfterPosition.X = 99;

        Assert.Equal(0, transform.BeforePosition.X);
        Assert.Equal(1, transform.AfterPosition.X);
    }

    [Fact]
    public void MissingPayload_IsRejected()
    {
        AssertRejected(null, "null");
        AssertRejected(new ViewportEvent(), "payload");
    }

    [Theory]
    [InlineData("")]
    [InlineData("  ")]
    public void TransformEvent_RejectsMissingIdentity(string instanceId)
    {
        var source = TransformPayload();
        source.Transform.InstanceId = instanceId;
        AssertRejected(source, "instance id");
    }

    [Theory]
    [InlineData(ViewportTransformOperation.Unspecified, ViewportTransformSpace.World)]
    [InlineData((ViewportTransformOperation)99, ViewportTransformSpace.World)]
    [InlineData(ViewportTransformOperation.Translate, ViewportTransformSpace.Unspecified)]
    [InlineData(ViewportTransformOperation.Translate, (ViewportTransformSpace)99)]
    public void TransformEvent_RejectsUnsupportedEnums(
        ViewportTransformOperation operation, ViewportTransformSpace space)
    {
        var source = TransformPayload();
        source.Transform.Operation = operation;
        source.Transform.Space = space;
        AssertRejected(source, "unsupported");
    }

    [Fact]
    public void TransformEvent_RejectsMissingAndNonFiniteVectors()
    {
        (string Name, Action<ViewportTransformEvent, Vector4?> Set)[] fields =
        [
            ("beforePosition", (t, v) => t.BeforePosition = v),
            ("beforeRotation", (t, v) => t.BeforeRotation = v),
            ("beforeScale", (t, v) => t.BeforeScale = v),
            ("afterPosition", (t, v) => t.AfterPosition = v),
            ("afterRotation", (t, v) => t.AfterRotation = v),
            ("afterScale", (t, v) => t.AfterScale = v)
        ];
        Action<Vector4, float>[] components =
        [
            (v, n) => v.X = n,
            (v, n) => v.Y = n,
            (v, n) => v.Z = n,
            (v, n) => v.W = n
        ];
        foreach (var field in fields)
        {
            var missing = TransformPayload();
            field.Set(missing.Transform, null);
            AssertRejected(missing, field.Name);

            foreach (var setComponent in components)
            foreach (var value in new[] { float.NaN, float.PositiveInfinity, float.NegativeInfinity })
            {
                var source = TransformPayload();
                var vector = new Vector4();
                setComponent(vector, value);
                field.Set(source.Transform, vector);
                AssertRejected(source, field.Name);
            }
        }
    }

    [Theory]
    [InlineData(0.25f, 0.75f)]
    [InlineData(0f, 1f)]
    [InlineData(1f, 0f)]
    public void AssetDropEvent_RoundTripsIdentityAndInclusiveCoordinates(float x, float y)
    {
        var source = AssetDropPayload(x, y);
        var drop = Assert.IsType<EditorViewportAssetDropEvent>(Create(source));

        Assert.Equal(31UL, drop.Revision);
        Assert.Equal(9UL, drop.ManagedMutationRevision);
        Assert.Equal(source.AssetDrop.FileId, drop.FileId);
        Assert.Equal(x, drop.NormalizedX);
        Assert.Equal(y, drop.NormalizedY);
    }

    [Theory]
    [InlineData("")]
    [InlineData("  ")]
    public void AssetDropEvent_RejectsMissingIdentity(string fileId)
    {
        var source = AssetDropPayload(0.5f, 0.5f);
        source.AssetDrop.FileId = fileId;
        AssertRejected(source, "file id");
    }

    [Theory]
    [InlineData(-0.1f)]
    [InlineData(1.1f)]
    [InlineData(float.NaN)]
    [InlineData(float.PositiveInfinity)]
    [InlineData(float.NegativeInfinity)]
    public void AssetDropEvent_RejectsInvalidCoordinatesOnEitherAxis(float value)
    {
        AssertRejected(AssetDropPayload(value, 0.5f), "normalized");
        AssertRejected(AssetDropPayload(0.5f, value), "normalized");
    }

    [Theory]
    [InlineData('Q')]
    [InlineData('W')]
    [InlineData('E')]
    [InlineData('R')]
    [InlineData('T')]
    public void ToolShortcutEvent_RoundTripsSupportedKeys(char key)
    {
        var shortcut = Assert.IsType<EditorViewportToolShortcutEvent>(Create(new ViewportEvent
        {
            Revision = 34,
            ManagedMutationRevision = 12,
            ToolShortcut = new ViewportToolShortcutEvent { KeyCode = key }
        }));

        Assert.Equal(34UL, shortcut.Revision);
        Assert.Equal(12UL, shortcut.ManagedMutationRevision);
        Assert.Equal((uint)key, shortcut.KeyCode);
    }

    [Theory]
    [InlineData((uint)'X')]
    [InlineData((uint)'w')]
    [InlineData(0u)]
    [InlineData(uint.MaxValue)]
    public void ToolShortcutEvent_RejectsUnsupportedKeys(uint key)
    {
        AssertRejected(new ViewportEvent
        {
            ToolShortcut = new ViewportToolShortcutEvent { KeyCode = key }
        }, "unsupported");
    }

    static EditorViewportEvent Create(ViewportEvent source)
    {
        var decoded = ViewportEvent.Parser.ParseFrom(source.ToByteArray());
        Assert.True(EditorViewportEventContract.TryCreate(decoded, out var result, out var error), error);
        Assert.Equal(string.Empty, error);
        return Assert.IsAssignableFrom<EditorViewportEvent>(result);
    }

    static void AssertRejected(ViewportEvent? source, string expectedError)
    {
        var decoded = source is null ? null : ViewportEvent.Parser.ParseFrom(source.ToByteArray());
        Assert.False(EditorViewportEventContract.TryCreate(decoded, out var result, out var error));
        Assert.Null(result);
        Assert.Contains(expectedError, error, StringComparison.OrdinalIgnoreCase);
    }

    static ViewportEvent TransformPayload() => new()
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

    static ViewportEvent AssetDropPayload(float x, float y) => new()
    {
        Revision = 31,
        ManagedMutationRevision = 9,
        AssetDrop = new ViewportAssetDropEvent
        {
            FileId = "{12345678-1234-1234-1234-123456789ABC}",
            NormalizedX = x,
            NormalizedY = y
        }
    };

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
