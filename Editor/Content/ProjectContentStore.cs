using SailorEditor.Services;
using SailorEditor.ViewModels;

namespace SailorEditor.Content;

public sealed class ProjectContentStore
{
    readonly AssetsService _assetsService;
    long _workspaceEpoch;

    public ProjectContentStore(AssetsService assetsService)
    {
        _assetsService = assetsService;
        _assetsService.Changed += OnAssetsChanged;
        Projection = BuildProjection(State);
    }

    public event Action<ProjectContentProjection>? ProjectionChanged;

    public ProjectContentState State { get; private set; } = new();
    public ProjectContentProjection Projection { get; private set; }
    public long WorkspaceEpoch => Interlocked.Read(ref _workspaceEpoch);

    public ProjectContentProjection Refresh() => Update(State, force: true);

    public ProjectContentProjection SelectFolder(int? folderId) => Update(State with
    {
        CurrentFolderId = folderId,
        SelectedAssetFileId = null,
        SelectedAssetPath = null
    });

    public ProjectContentProjection SelectAsset(AssetFile? assetFile)
    {
        var fullPath = assetFile?.AssetInfo?.FullName ?? assetFile?.Asset?.FullName;
        var asset = CreateAssetSnapshots().FirstOrDefault(x =>
            string.Equals(
                string.IsNullOrWhiteSpace(x.IdentityPath) ? x.FullPath : x.IdentityPath,
                fullPath,
                ProjectContentPathPolicy.PathComparison));
        var folderId = asset is null ? State.CurrentFolderId : asset.FolderId == -1 ? null : asset.FolderId;
        return Update(State with
        {
            CurrentFolderId = folderId,
            SelectedAssetFileId = asset?.FileId,
            SelectedAssetPath = asset is null
                ? null
                : (string.IsNullOrWhiteSpace(asset.IdentityPath) ? asset.FullPath : asset.IdentityPath)
        });
    }

    public ProjectContentProjection SetFilter(string? filter) => Update(State with { Filter = filter });

    public ProjectContentProjection SetSort(ProjectContentSortMode sortMode) => Update(State with { SortMode = sortMode });

    public ProjectContentProjection ResetForWorkspaceChange()
    {
        Interlocked.Increment(ref _workspaceEpoch);
        return Update(new ProjectContentState(), force: true);
    }

    void OnAssetsChanged() => Update(State, force: true);

    ProjectContentProjection Update(ProjectContentState state, bool force = false)
    {
        if (!force && state == State)
            return Projection;

        Projection = BuildProjection(state);
        State = Projection.State;
        ProjectionChanged?.Invoke(Projection);
        return Projection;
    }

    ProjectContentProjection BuildProjection(ProjectContentState state) => ProjectContentProjectionBuilder.Build(
        _assetsService.Root.Name,
        CreateFolderSnapshots(),
        CreateAssetSnapshots(),
        state);

    IReadOnlyList<ProjectContentFolderSnapshot> CreateFolderSnapshots() => _assetsService.Folders
        .Select(x => new ProjectContentFolderSnapshot(x.Id, x.Name, x.ParentFolderId, x.FullPath, x.IsReadOnly))
        .ToArray();

    IReadOnlyList<ProjectContentAssetSnapshot> CreateAssetSnapshots() => _assetsService.Files
        .Where(x => x.FileId is not null && !x.FileId.IsEmpty())
        .Select(x => new ProjectContentAssetSnapshot(
            x.FileId!.Value,
            x.DisplayName,
            x.Asset?.Extension ?? string.Empty,
            x.FolderId,
            x.Asset?.FullName ?? x.AssetInfo?.FullName ?? x.DisplayName,
            x.AssetInfoTypeName,
            x.AssetInfo?.FullName ?? string.Empty,
            x.ProjectRootId,
            x.IsReadOnly))
        .ToArray();
}
