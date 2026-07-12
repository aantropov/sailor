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
}
