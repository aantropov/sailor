namespace SailorEngine;

public enum EditorComponentPropertyAccess
{
    Writable,
    ReadOnly,
    Unknown
}

public static class EditorComponentPropertyContract
{
    public static EditorComponentPropertyAccess Classify<TProperty>(
        string propertyName,
        IReadOnlyDictionary<string, TProperty> writableProperties,
        IReadOnlySet<string> readOnlyProperties)
    {
        if (writableProperties.ContainsKey(propertyName))
            return EditorComponentPropertyAccess.Writable;
        if (readOnlyProperties.Contains(propertyName))
            return EditorComponentPropertyAccess.ReadOnly;

        return EditorComponentPropertyAccess.Unknown;
    }

    public static string ValidateEnumValue(
        string componentTypeName,
        string propertyName,
        string enumTypeName,
        string value,
        IReadOnlyCollection<string> allowedValues)
    {
        if (!allowedValues.Contains(value, StringComparer.Ordinal))
        {
            throw new InvalidDataException(
                $"Invalid value '{value}' for enum property '{componentTypeName}.{propertyName}' " +
                $"of type '{enumTypeName}'.");
        }

        return value;
    }
}
