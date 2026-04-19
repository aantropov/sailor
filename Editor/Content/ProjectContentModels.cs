namespace SailorEditor.Content;

public sealed record ProjectContentFolderSnapshot(int Id, string Name, int ParentFolderId);
public sealed record ProjectContentAssetSnapshot(string FileId, string DisplayName, string Extension, int FolderId, string FullPath, string? AssetInfoTypeName = null);

public enum ProjectContentSortMode
{
    NameAscending,
    NameDescending,
}

public sealed record ProjectContentState(
    int? CurrentFolderId = null,
    string? SelectedAssetFileId = null,
    string? Filter = null,
    ProjectContentSortMode SortMode = ProjectContentSortMode.NameAscending);

public sealed record ProjectContentFolderItem(int? FolderId, string Name, string FullPath, int? ParentFolderId, bool IsRoot = false);

public sealed record ProjectContentAssetItem(string FileId, string DisplayName, string Extension, int FolderId, string FullPath, string? AssetInfoTypeName = null);

public sealed record ProjectContentProjection(
    ProjectContentFolderItem Root,
    IReadOnlyList<ProjectContentFolderItem> Folders,
    IReadOnlyList<ProjectContentAssetItem> VisibleAssets,
    ProjectContentState State)
{
    public IReadOnlyList<ProjectContentAssetItem> CurrentFolderAssets => State.CurrentFolderId is { } folderId
        ? VisibleAssets.Where(x => x.FolderId == folderId).ToArray()
        : VisibleAssets.Where(x => x.FolderId == -1).ToArray();
}

public static class ProjectContentProjectionBuilder
{
    public static ProjectContentProjection Build(
        string rootPath,
        IReadOnlyList<ProjectContentFolderSnapshot> folders,
        IReadOnlyList<ProjectContentAssetSnapshot> assets,
        ProjectContentState state)
    {
        var normalizedState = NormalizeState(folders, state);
        var folderItems = folders
            .Select(folder => new ProjectContentFolderItem(folder.Id, folder.Name, BuildFolderPath(rootPath, folder, folders), folder.ParentFolderId))
            .OrderBy(x => x.FullPath, StringComparer.OrdinalIgnoreCase)
            .ToArray();

        var assetItems = assets
            .Where(asset => !string.IsNullOrWhiteSpace(asset.FileId))
            .Select(asset => new ProjectContentAssetItem(asset.FileId, asset.DisplayName, asset.Extension, asset.FolderId, asset.FullPath, asset.AssetInfoTypeName))
            .Where(asset => MatchesFolder(asset, normalizedState.CurrentFolderId) && MatchesFilter(asset, normalizedState.Filter))
            .OrderBy(asset => asset.DisplayName, StringComparer.OrdinalIgnoreCase)
            .ToArray();

        if (normalizedState.SortMode == ProjectContentSortMode.NameDescending)
            assetItems = assetItems.Reverse().ToArray();

        return new ProjectContentProjection(
            new ProjectContentFolderItem(null, Path.GetFileName(rootPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)), rootPath, null, true),
            folderItems,
            assetItems,
            normalizedState);
    }

    static bool MatchesFolder(ProjectContentAssetItem asset, int? folderId) => (folderId ?? -1) == asset.FolderId;

    static bool MatchesFilter(ProjectContentAssetItem asset, string? filter)
    {
        if (string.IsNullOrWhiteSpace(filter))
            return true;

        return asset.DisplayName.Contains(filter, StringComparison.OrdinalIgnoreCase)
            || asset.Extension.Contains(filter, StringComparison.OrdinalIgnoreCase)
            || (asset.AssetInfoTypeName?.Contains(filter, StringComparison.OrdinalIgnoreCase) ?? false);
    }

    static ProjectContentState NormalizeState(IReadOnlyList<ProjectContentFolderSnapshot> folders, ProjectContentState state)
    {
        var folderExists = state.CurrentFolderId is null or -1 || folders.Any(x => x.Id == state.CurrentFolderId);
        return folderExists ? state : state with { CurrentFolderId = null };
    }

    static string BuildFolderPath(string rootPath, ProjectContentFolderSnapshot folder, IReadOnlyList<ProjectContentFolderSnapshot> folders)
    {
        var parts = new Stack<string>();
        ProjectContentFolderSnapshot? current = folder;
        while (current is not null)
        {
            parts.Push(current.Name);
            current = folders.FirstOrDefault(x => x.Id == current.ParentFolderId);
        }

        return Path.Combine(rootPath, Path.Combine(parts.ToArray()));
    }
}
