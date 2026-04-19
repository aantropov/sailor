namespace SailorEditor.Content;

public static class ProjectContentFolderIds
{
    public static int FromRelativePath(string relativePath)
    {
        var normalized = Normalize(relativePath);
        if (normalized.Length == 0)
            return 1;

        const uint offsetBasis = 2166136261;
        const uint prime = 16777619;

        uint hash = offsetBasis;
        foreach (var ch in normalized)
        {
            hash ^= ch;
            hash *= prime;
        }

        var id = (int)(hash & 0x7fffffff);
        if (id is 0 or -1)
            id = 1;

        return id;
    }

    static string Normalize(string relativePath) => relativePath
        .Replace('\\', Path.DirectorySeparatorChar)
        .Replace('/', Path.DirectorySeparatorChar)
        .Trim(Path.DirectorySeparatorChar)
        .ToLowerInvariant();
}
