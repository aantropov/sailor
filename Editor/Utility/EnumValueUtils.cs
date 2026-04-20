namespace SailorEditor.Utility;

public static class EnumValueUtils
{
    public static string Normalize(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return string.Empty;
        }

        var normalized = value.Trim();
        var separatorIndex = normalized.LastIndexOf("::", StringComparison.Ordinal);
        return separatorIndex >= 0 ? normalized[(separatorIndex + 2)..] : normalized;
    }
}
