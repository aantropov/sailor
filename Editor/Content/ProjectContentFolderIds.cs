namespace SailorEditor.Content;

public static class ProjectContentFolderIds
{
    public const int WorkspaceContentRootId = int.MaxValue - 1;
    public const int EngineContentRootId = int.MaxValue - 2;

    public static int FromRelativePath(string relativePath)
        => FromNormalizedPath(Normalize(relativePath));

    public static int FromRootedRelativePath(int rootId, string relativePath)
        => FromNormalizedPath($"{rootId}:{Normalize(relativePath)}");

    static int FromNormalizedPath(string normalized)
    {
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
        if (id is WorkspaceContentRootId or EngineContentRootId)
            id -= 3;

        return id;
    }

    static string Normalize(string relativePath) => relativePath
        .Replace('\\', Path.DirectorySeparatorChar)
        .Replace('/', Path.DirectorySeparatorChar)
        .Trim(Path.DirectorySeparatorChar)
        .ToLowerInvariant();
}

public sealed class ProjectContentFolderIdAllocator
{
    readonly HashSet<int> _used =
    [
        0,
        -1,
        ProjectContentFolderIds.WorkspaceContentRootId,
        ProjectContentFolderIds.EngineContentRootId
    ];

    public int Allocate(int rootId, string relativePath, bool useRootedFolderIds)
    {
        var candidate = useRootedFolderIds
            ? ProjectContentFolderIds.FromRootedRelativePath(rootId, relativePath)
            : ProjectContentFolderIds.FromRelativePath(relativePath);

        while (!_used.Add(candidate))
            candidate = Next(candidate);

        return candidate;
    }

    public void Reserve(int id) => _used.Add(id);

    static int Next(int id) => id >= int.MaxValue - 3 ? 1 : id + 1;
}
