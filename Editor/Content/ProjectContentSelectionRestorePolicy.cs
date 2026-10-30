using SailorEditor.Services;
using SailorEditor.ViewModels;

namespace SailorEditor.Content;

public static class ProjectContentSelectionRestorePolicy
{
    public static async Task<AssetFile?> ResolveAssetAsync(
        AssetsService assets,
        AssetFile original,
        IReadOnlyDictionary<string, string> folderPaths,
        CancellationToken cancellationToken = default)
    {
        var epoch = assets.WorkspaceEpoch;
        var directory = original.AssetInfo?.DirectoryName ?? original.Asset?.DirectoryName;
        if (!string.IsNullOrWhiteSpace(directory))
        {
            foreach (var mapping in folderPaths)
            {
                if (ProjectContentPathPolicy.IsInsideRoot(mapping.Key, directory))
                {
                    var mapped = ProjectContentFolderRebindPolicy.CreatePlan(directory, mapping.Key, mapping.Value,
                        ProjectContentFolderMutationKind.MoveOrRename).SuccessPath!;
                    if (Directory.Exists(mapped))
                    {
                        directory = mapped;
                    }
                    break;
                }
            }
            var folder = await assets.ResolveFolderAsync(directory, cancellationToken);
            if (epoch != assets.WorkspaceEpoch)
            {
                return null;
            }
            if (folder is not null)
            {
                await assets.EnsureFolderLoadedAsync(folder.Id, cancellationToken);
            }
        }
        var refreshed = await assets.ResolveAssetAsync(original.FileId, cancellationToken);
        if (epoch != assets.WorkspaceEpoch)
        {
            return null;
        }
        if (!original.IsDirty)
        {
            return refreshed;
        }
        if (refreshed is null)
        {
            throw new InvalidOperationException($"Cannot restore pending edits for asset '{original.FileId}' after the Content operation: its metadata location could not be resolved.");
        }
        original.RebindLocation(refreshed);
        return original;
    }
}
