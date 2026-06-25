namespace SailorEditor.Workspace;

public sealed record WorkspaceRecentItem(
    string Name,
    string ManifestPath,
    string DisplayPath,
    DateTimeOffset LastOpenedAt);

public sealed record WorkspaceUiProjection(
    string ActiveWorkspaceName,
    string ActiveWorkspacePath,
    bool HasActiveWorkspace,
    IReadOnlyList<WorkspaceRecentItem> RecentWorkspaces)
{
    public static WorkspaceUiProjection Empty { get; } = new("No workspace", "Repository fallback", false, []);

    public string ToolbarText => HasActiveWorkspace ? ActiveWorkspaceName : "No workspace";
}

public static class WorkspaceUiProjectionBuilder
{
    public static WorkspaceUiProjection Build(WorkspaceSession? current, IReadOnlyList<RecentWorkspaceEntry> recentWorkspaces)
    {
        var recent = recentWorkspaces
            .Where(x => !string.IsNullOrWhiteSpace(x.ManifestPath))
            .Select(x => new WorkspaceRecentItem(
                string.IsNullOrWhiteSpace(x.Name) ? Path.GetFileNameWithoutExtension(x.ManifestPath) : x.Name,
                Path.GetFullPath(x.ManifestPath),
                CompactPath(x.ManifestPath),
                x.LastOpenedAt))
            .ToArray();

        if (current is null)
            return WorkspaceUiProjection.Empty with { RecentWorkspaces = recent };

        return new WorkspaceUiProjection(
            string.IsNullOrWhiteSpace(current.Manifest.Name) ? Path.GetFileName(current.WorkspaceRoot) : current.Manifest.Name,
            current.WorkspaceRoot,
            true,
            recent);
    }

    static string CompactPath(string path)
    {
        try
        {
            var fullPath = Path.GetFullPath(path);
            var directory = Path.GetDirectoryName(fullPath);
            return string.IsNullOrWhiteSpace(directory)
                ? fullPath
                : Path.Combine(Path.GetFileName(directory), Path.GetFileName(fullPath));
        }
        catch
        {
            return path;
        }
    }
}
