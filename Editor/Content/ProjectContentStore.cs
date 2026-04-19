using SailorEditor.Services;

namespace SailorEditor.Content;

public sealed class ProjectContentStore
{
    readonly AssetsService _assetsService;

    public ProjectContentStore(AssetsService assetsService)
    {
        _assetsService = assetsService;
        Projection = ProjectContentProjectionBuilder.Build(_assetsService.Root.Name, CreateFolderSnapshots(), CreateAssetSnapshots(), State);
    }

    public ProjectContentState State { get; private set; } = new();
    public ProjectContentProjection Projection { get; private set; }

    public ProjectContentProjection Refresh()
    {
        Projection = ProjectContentProjectionBuilder.Build(_assetsService.Root.Name, CreateFolderSnapshots(), CreateAssetSnapshots(), State);
        return Projection;
    }

    public ProjectContentProjection SelectFolder(int? folderId)
    {
        State = State with { CurrentFolderId = folderId };
        return Refresh();
    }

    public ProjectContentProjection SelectAsset(string? fileId)
    {
        State = State with { SelectedAssetFileId = fileId };
        return Refresh();
    }

    public ProjectContentProjection SetFilter(string? filter)
    {
        State = State with { Filter = filter };
        return Refresh();
    }

    public ProjectContentProjection SetSort(ProjectContentSortMode sortMode)
    {
        State = State with { SortMode = sortMode };
        return Refresh();
    }

    IReadOnlyList<ProjectContentFolderSnapshot> CreateFolderSnapshots() => _assetsService.Folders
        .Select(x => new ProjectContentFolderSnapshot(x.Id, x.Name, x.ParentFolderId))
        .ToArray();

    IReadOnlyList<ProjectContentAssetSnapshot> CreateAssetSnapshots() => _assetsService.Files
        .Where(x => x.FileId is not null && !x.FileId.IsEmpty())
        .Select(x => new ProjectContentAssetSnapshot(x.FileId!.Value, x.DisplayName, x.Asset?.Extension ?? string.Empty, x.FolderId, x.Asset?.FullName ?? x.AssetInfo?.FullName ?? x.DisplayName, x.AssetInfoTypeName))
        .ToArray();
}
