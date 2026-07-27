#nullable enable
using System.Globalization;
using SailorEditor.Protocol.Generated;
using YamlDotNet.RepresentationModel;

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

            default:
                error = "The viewport event does not contain a supported payload.";
                return false;
        }
    }

    static readonly HashSet<string> SelectionFields =
    [
        "kind",
        "revision",
        "managedMutationRevision",
        "selectedInstanceId",
    ];

    static readonly HashSet<string> TransformFields =
    [
        "kind",
        "revision",
        "managedMutationRevision",
        "instanceId",
        "operation",
        "space",
        "beforePosition",
        "beforeRotation",
        "beforeScale",
        "afterPosition",
        "afterRotation",
        "afterScale",
    ];

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

    public static bool TryParse(string yaml, out EditorViewportEvent? viewportEvent, out string error)
    {
        viewportEvent = null;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(yaml))
        {
            error = "The viewport event payload is empty.";
            return false;
        }

        Dictionary<string, YamlNode> fields;
        try
        {
            using var reader = new StringReader(yaml);
            var stream = new YamlStream();
            stream.Load(reader);
            if (stream.Documents.Count != 1 || stream.Documents[0].RootNode is not YamlMappingNode mapping)
            {
                error = "The viewport event payload must contain one YAML mapping.";
                return false;
            }

            fields = new Dictionary<string, YamlNode>(StringComparer.Ordinal);
            foreach (var pair in mapping.Children)
            {
                if (pair.Key is not YamlScalarNode key || string.IsNullOrWhiteSpace(key.Value))
                {
                    error = "The viewport event contains a non-scalar or empty field name.";
                    return false;
                }

                if (!fields.TryAdd(key.Value, pair.Value))
                {
                    error = $"The viewport event contains duplicate field '{key.Value}'.";
                    return false;
                }
            }
        }
        catch (Exception ex)
        {
            error = $"The viewport event is not valid YAML: {ex.Message}";
            return false;
        }

        if (!TryReadScalar(fields, "kind", allowEmpty: false, out var kind, out error) ||
            !TryReadUnsignedInteger(fields, "revision", out var revision, out error) ||
            !TryReadUnsignedInteger(fields, "managedMutationRevision", out var managedMutationRevision, out error))
        {
            return false;
        }

        switch (kind)
        {
            case "selection":
                if (!ValidateFields(fields, SelectionFields, out error) ||
                    !TryReadScalar(fields, "selectedInstanceId", allowEmpty: true, out var selectedInstanceId, out error))
                {
                    return false;
                }

                viewportEvent = new EditorViewportSelectionEvent(revision, managedMutationRevision, selectedInstanceId);
                return true;

            case "transform":
                if (!ValidateFields(fields, TransformFields, out error) ||
                    !TryReadScalar(fields, "instanceId", allowEmpty: false, out var instanceId, out error) ||
                    !TryReadEnum(fields, "operation", out EditorViewportTransformOperation operation, out error) ||
                    !TryReadEnum(fields, "space", out EditorViewportTransformSpace space, out error) ||
                    !TryReadVector(fields, "beforePosition", out var beforePosition, out error) ||
                    !TryReadVector(fields, "beforeRotation", out var beforeRotation, out error) ||
                    !TryReadVector(fields, "beforeScale", out var beforeScale, out error) ||
                    !TryReadVector(fields, "afterPosition", out var afterPosition, out error) ||
                    !TryReadVector(fields, "afterRotation", out var afterRotation, out error) ||
                    !TryReadVector(fields, "afterScale", out var afterScale, out error))
                {
                    return false;
                }

                viewportEvent = new EditorViewportTransformEvent(
                    revision,
                    managedMutationRevision,
                    instanceId,
                    operation,
                    space,
                    beforePosition,
                    beforeRotation,
                    beforeScale,
                    afterPosition,
                    afterRotation,
                    afterScale);
                return true;

            default:
                error = $"Unsupported viewport event kind '{kind}'.";
                return false;
        }
    }

    static bool ValidateFields(
        IReadOnlyDictionary<string, YamlNode> fields,
        IReadOnlySet<string> expected,
        out string error)
    {
        var unexpected = fields.Keys.FirstOrDefault(field => !expected.Contains(field));
        if (unexpected is not null)
        {
            error = $"The viewport event contains unexpected field '{unexpected}'.";
            return false;
        }

        var missing = expected.FirstOrDefault(field => !fields.ContainsKey(field));
        if (missing is not null)
        {
            error = $"The viewport event is missing required field '{missing}'.";
            return false;
        }

        error = string.Empty;
        return true;
    }

    static bool TryReadUnsignedInteger(
        IReadOnlyDictionary<string, YamlNode> fields,
        string field,
        out ulong value,
        out string error)
    {
        value = 0;
        if (!TryReadScalar(fields, field, allowEmpty: false, out var scalar, out error))
        {
            return false;
        }

        if (!ulong.TryParse(scalar, NumberStyles.None, CultureInfo.InvariantCulture, out value))
        {
            error = $"Viewport event field '{field}' must be an unsigned integer.";
            return false;
        }

        return true;
    }

    static bool TryReadScalar(
        IReadOnlyDictionary<string, YamlNode> fields,
        string field,
        bool allowEmpty,
        out string value,
        out string error)
    {
        value = string.Empty;
        if (!fields.TryGetValue(field, out var node))
        {
            error = $"The viewport event is missing required field '{field}'.";
            return false;
        }

        if (node is not YamlScalarNode scalar)
        {
            error = $"Viewport event field '{field}' must be a scalar.";
            return false;
        }

        value = scalar.Value ?? string.Empty;
        if (!allowEmpty && string.IsNullOrWhiteSpace(value))
        {
            error = $"Viewport event field '{field}' must not be empty.";
            return false;
        }

        error = string.Empty;
        return true;
    }

    static bool TryReadEnum<TEnum>(
        IReadOnlyDictionary<string, YamlNode> fields,
        string field,
        out TEnum value,
        out string error)
        where TEnum : struct, Enum
    {
        value = default;
        if (!TryReadScalar(fields, field, allowEmpty: false, out var scalar, out error))
        {
            return false;
        }

        if (!Enum.TryParse(scalar, ignoreCase: false, out value) ||
            !Enum.IsDefined(value) ||
            !string.Equals(value.ToString(), scalar, StringComparison.Ordinal))
        {
            error = $"Viewport event field '{field}' has unsupported value '{scalar}'.";
            return false;
        }

        return true;
    }

    static bool TryReadVector(
        IReadOnlyDictionary<string, YamlNode> fields,
        string field,
        out EditorViewportVector4 value,
        out string error)
    {
        value = default;
        if (!fields.TryGetValue(field, out var node))
        {
            error = $"The viewport event is missing required field '{field}'.";
            return false;
        }

        if (node is not YamlSequenceNode sequence || sequence.Children.Count != 4)
        {
            error = $"Viewport event field '{field}' must contain exactly four numbers.";
            return false;
        }

        Span<float> components = stackalloc float[4];
        for (var i = 0; i < components.Length; ++i)
        {
            if (sequence.Children[i] is not YamlScalarNode scalar ||
                !float.TryParse(scalar.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out components[i]) ||
                !float.IsFinite(components[i]))
            {
                error = $"Viewport event field '{field}' contains a non-finite or invalid number at index {i}.";
                return false;
            }
        }

        value = new EditorViewportVector4(components[0], components[1], components[2], components[3]);
        error = string.Empty;
        return true;
    }
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
