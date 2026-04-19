using SailorEditor.Services;

namespace SailorEditor.Content;

public sealed class ProjectContentStore
{
    readonly AssetsService _assetsService;

    public ProjectContentStore(AssetsService assetsService)
    {
        _assetsService = assetsService;
        _assetsService.Changed += OnAssetsChanged;
        Projection = BuildProjection(State);
    }

    public event Action<ProjectContentProjection>? ProjectionChanged;

    public ProjectContentState State { get; private set; } = new();
    public ProjectContentProjection Projection { get; private set; }

    public ProjectContentProjection Refresh() => Update(State, force: true);

    public ProjectContentProjection SelectFolder(int? folderId) => Update(State with { CurrentFolderId = folderId, SelectedAssetFileId = null });

    public ProjectContentProjection SelectAsset(string? fileId)
    {
        var asset = CreateAssetSnapshots().FirstOrDefault(x => x.FileId == fileId);
        var folderId = asset is null ? State.CurrentFolderId : asset.FolderId == -1 ? null : asset.FolderId;
        return Update(State with { CurrentFolderId = folderId, SelectedAssetFileId = fileId });
    }

    public ProjectContentProjection SetFilter(string? filter) => Update(State with { Filter = filter });

    public ProjectContentProjection SetSort(ProjectContentSortMode sortMode) => Update(State with { SortMode = sortMode });

    void OnAssetsChanged() => Update(State, force: true);

    ProjectContentProjection Update(ProjectContentState state, bool force = false)
    {
        if (!force && state == State)
            return Projection;

        State = state;
        Projection = BuildProjection(State);
        ProjectionChanged?.Invoke(Projection);
        return Projection;
    }

    ProjectContentProjection BuildProjection(ProjectContentState state) => ProjectContentProjectionBuilder.Build(
        _assetsService.Root.Name,
        CreateFolderSnapshots(),
        CreateAssetSnapshots(),
        state);

    IReadOnlyList<ProjectContentFolderSnapshot> CreateFolderSnapshots() => _assetsService.Folders
        .Select(x => new ProjectContentFolderSnapshot(x.Id, x.Name, x.ParentFolderId))
        .ToArray();

    IReadOnlyList<ProjectContentAssetSnapshot> CreateAssetSnapshots() => _assetsService.Files
        .Where(x => x.FileId is not null && !x.FileId.IsEmpty())
        .Select(x => new ProjectContentAssetSnapshot(x.FileId!.Value, x.DisplayName, x.Asset?.Extension ?? string.Empty, x.FolderId, x.Asset?.FullName ?? x.AssetInfo?.FullName ?? x.DisplayName, x.AssetInfoTypeName))
        .ToArray();
}
