#nullable enable
using SailorEditor.Protocol.Generated;

namespace SailorEditor.Scene;

public enum EditorViewportTransformOperation
{
    Select,
    Translate,
    Rotate,
    Scale,
}

public enum EditorViewportTransformSpace
{
    World,
    Local,
}

public readonly record struct EditorViewportVector4(float X, float Y, float Z, float W);

public static class EditorViewportMutationOrder
{
    public static bool IsCurrent(ulong eventManagedMutationRevision, ulong currentManagedMutationRevision) =>
        eventManagedMutationRevision == currentManagedMutationRevision;
}

public abstract record EditorViewportEvent(ulong Revision, ulong ManagedMutationRevision);

public sealed record EditorViewportSelectionEvent(
    ulong Revision,
    ulong ManagedMutationRevision,
    string SelectedInstanceId)
    : EditorViewportEvent(Revision, ManagedMutationRevision);

public sealed record EditorViewportTransformEvent(
    ulong Revision,
    ulong ManagedMutationRevision,
    string InstanceId,
    EditorViewportTransformOperation Operation,
    EditorViewportTransformSpace Space,
    EditorViewportVector4 BeforePosition,
    EditorViewportVector4 BeforeRotation,
    EditorViewportVector4 BeforeScale,
    EditorViewportVector4 AfterPosition,
    EditorViewportVector4 AfterRotation,
    EditorViewportVector4 AfterScale)
    : EditorViewportEvent(Revision, ManagedMutationRevision);

public sealed record EditorViewportAssetDropEvent(
    ulong Revision,
    ulong ManagedMutationRevision,
    string FileId,
    float NormalizedX,
    float NormalizedY)
    : EditorViewportEvent(Revision, ManagedMutationRevision);

public sealed record EditorViewportToolShortcutEvent(
    ulong Revision,
    ulong ManagedMutationRevision,
    uint KeyCode)
    : EditorViewportEvent(Revision, ManagedMutationRevision);

public static class EditorViewportEventContract
{
    public static bool TryCreate(
        ViewportEvent? source,
        out EditorViewportEvent? viewportEvent,
        out string error)
    {
        viewportEvent = null;
        error = string.Empty;
        if (source is null)
        {
            error = "The viewport event payload is null.";
            return false;
        }

        switch (source.PayloadCase)
        {
            case ViewportEvent.PayloadOneofCase.Selection:
                viewportEvent = new EditorViewportSelectionEvent(
                    source.Revision,
                    source.ManagedMutationRevision,
                    source.Selection.SelectedInstanceId);
                return true;

            case ViewportEvent.PayloadOneofCase.Transform:
                var transform = source.Transform;
                if (string.IsNullOrWhiteSpace(transform.InstanceId))
                {
                    error = "Viewport transform event instance id must not be empty.";
                    return false;
                }

                if (!TryMapOperation(transform.Operation, out var operation) ||
                    !TryMapSpace(transform.Space, out var space))
                {
                    error = "The viewport transform event contains an unsupported operation or space.";
                    return false;
                }

                if (!TryCreateVector(transform.BeforePosition, "beforePosition", out var beforePosition, out error) ||
                    !TryCreateVector(transform.BeforeRotation, "beforeRotation", out var beforeRotation, out error) ||
                    !TryCreateVector(transform.BeforeScale, "beforeScale", out var beforeScale, out error) ||
                    !TryCreateVector(transform.AfterPosition, "afterPosition", out var afterPosition, out error) ||
                    !TryCreateVector(transform.AfterRotation, "afterRotation", out var afterRotation, out error) ||
                    !TryCreateVector(transform.AfterScale, "afterScale", out var afterScale, out error))
                {
                    return false;
                }

                viewportEvent = new EditorViewportTransformEvent(
                    source.Revision,
                    source.ManagedMutationRevision,
                    transform.InstanceId,
                    operation,
                    space,
                    beforePosition,
                    beforeRotation,
                    beforeScale,
                    afterPosition,
                    afterRotation,
                    afterScale);
                return true;

            case ViewportEvent.PayloadOneofCase.AssetDrop:
                var assetDrop = source.AssetDrop;
                if (string.IsNullOrWhiteSpace(assetDrop.FileId))
                {
                    error = "Viewport asset-drop event file id must not be empty.";
                    return false;
                }

                if (!IsNormalized(assetDrop.NormalizedX) ||
                    !IsNormalized(assetDrop.NormalizedY))
                {
                    error =
                        "Viewport asset-drop coordinates must be finite and normalized to [0, 1].";
                    return false;
                }

                viewportEvent = new EditorViewportAssetDropEvent(
                    source.Revision,
                    source.ManagedMutationRevision,
                    assetDrop.FileId,
                    assetDrop.NormalizedX,
                    assetDrop.NormalizedY);
                return true;

            case ViewportEvent.PayloadOneofCase.ToolShortcut:
                var keyCode = source.ToolShortcut.KeyCode;
                if (!IsToolShortcutKey(keyCode))
                {
                    error =
                        "The viewport tool shortcut event contains an unsupported key.";
                    return false;
                }

                viewportEvent = new EditorViewportToolShortcutEvent(
                    source.Revision,
                    source.ManagedMutationRevision,
                    keyCode);
                return true;

            default:
                error = "The viewport event does not contain a supported payload.";
                return false;
        }
    }

    static bool TryMapOperation(
        ViewportTransformOperation source,
        out EditorViewportTransformOperation operation)
    {
        switch (source)
        {
            case ViewportTransformOperation.Select:
                operation = EditorViewportTransformOperation.Select;
                return true;
            case ViewportTransformOperation.Translate:
                operation = EditorViewportTransformOperation.Translate;
                return true;
            case ViewportTransformOperation.Rotate:
                operation = EditorViewportTransformOperation.Rotate;
                return true;
            case ViewportTransformOperation.Scale:
                operation = EditorViewportTransformOperation.Scale;
                return true;
            default:
                operation = default;
                return false;
        }
    }

    static bool TryMapSpace(
        ViewportTransformSpace source,
        out EditorViewportTransformSpace space)
    {
        switch (source)
        {
            case ViewportTransformSpace.World:
                space = EditorViewportTransformSpace.World;
                return true;
            case ViewportTransformSpace.Local:
                space = EditorViewportTransformSpace.Local;
                return true;
            default:
                space = default;
                return false;
        }
    }

    static bool TryCreateVector(
        Vector4? source,
        string field,
        out EditorViewportVector4 value,
        out string error)
    {
        value = default;
        if (source is null)
        {
            error = $"The viewport event is missing required field '{field}'.";
            return false;
        }

        if (!float.IsFinite(source.X) ||
            !float.IsFinite(source.Y) ||
            !float.IsFinite(source.Z) ||
            !float.IsFinite(source.W))
        {
            error = $"Viewport event field '{field}' contains a non-finite number.";
            return false;
        }

        value = new EditorViewportVector4(source.X, source.Y, source.Z, source.W);
        error = string.Empty;
        return true;
    }

    static bool IsNormalized(float value)
        => float.IsFinite(value) && value >= 0.0f && value <= 1.0f;

    static bool IsToolShortcutKey(uint keyCode)
        => keyCode is 'Q' or 'W' or 'E' or 'R' or 'T';
}

public sealed class EditorViewportEventRevisionGate
{
    readonly object _sync = new();
    bool _hasAcceptedRevision;
    ulong _lastAcceptedRevision;

    public ulong? LastAcceptedRevision
    {
        get
        {
            lock (_sync)
            {
                return _hasAcceptedRevision ? _lastAcceptedRevision : null;
            }
        }
    }

    public bool TryAccept(EditorViewportEvent viewportEvent)
    {
        ArgumentNullException.ThrowIfNull(viewportEvent);
        lock (_sync)
        {
            if (_hasAcceptedRevision && viewportEvent.Revision <= _lastAcceptedRevision)
            {
                return false;
            }

            _hasAcceptedRevision = true;
            _lastAcceptedRevision = viewportEvent.Revision;
            return true;
        }
    }

    public bool IsCurrent(EditorViewportEvent viewportEvent)
    {
        ArgumentNullException.ThrowIfNull(viewportEvent);
        lock (_sync)
        {
            return _hasAcceptedRevision &&
                viewportEvent.Revision == _lastAcceptedRevision;
        }
    }

    public void Reset()
    {
        lock (_sync)
        {
            _hasAcceptedRevision = false;
            _lastAcceptedRevision = 0;
        }
    }
}

public sealed class EditorViewportEventEpochGate
{
    long _epoch;

    public long Current => Volatile.Read(ref _epoch);

    public long Advance() => Interlocked.Increment(ref _epoch);

    public bool IsCurrent(long epoch) => epoch == Current;
}
