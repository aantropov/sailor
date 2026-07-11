#nullable enable

using System.Globalization;

namespace SailorEngine;

public enum EditorComponentScalarKind
{
    String,
    Boolean,
    Int32,
    UInt32,
    Float
}

public static class EditorComponentScalarCodec
{
    public static object Parse(EditorComponentScalarKind kind, string? value)
    {
        var scalar = value ?? string.Empty;
        return kind switch
        {
            EditorComponentScalarKind.String => scalar,
            EditorComponentScalarKind.Boolean when bool.TryParse(scalar, out var parsed) => parsed,
            EditorComponentScalarKind.Int32 when int.TryParse(
                scalar,
                NumberStyles.Integer,
                CultureInfo.InvariantCulture,
                out var parsed) => parsed,
            EditorComponentScalarKind.UInt32 when uint.TryParse(
                scalar,
                NumberStyles.Integer,
                CultureInfo.InvariantCulture,
                out var parsed) => parsed,
            EditorComponentScalarKind.Float when float.TryParse(
                scalar,
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out var parsed) => parsed,
            _ => throw new InvalidDataException($"Invalid {kind} scalar value '{scalar}'.")
        };
    }

    public static string Format(EditorComponentScalarKind kind, object? value)
    {
        return kind switch
        {
            EditorComponentScalarKind.String when value is string stringValue => stringValue,
            EditorComponentScalarKind.Boolean when value is bool boolValue => boolValue ? "true" : "false",
            EditorComponentScalarKind.Int32 when value is int intValue => intValue.ToString(CultureInfo.InvariantCulture),
            EditorComponentScalarKind.UInt32 when value is uint uintValue => uintValue.ToString(CultureInfo.InvariantCulture),
            EditorComponentScalarKind.Float when value is float floatValue => floatValue.ToString("R", CultureInfo.InvariantCulture),
            _ => throw new InvalidDataException($"Value '{value}' does not match scalar kind {kind}.")
        };
    }
}
