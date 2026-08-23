namespace SailorEditor.Services;

public readonly record struct WorldAssetRebindResult<TAsset>(
    TAsset? Asset,
    bool IsUntitled,
    bool? DirtyState)
    where TAsset : class;

public static class WorldAssetRebindPolicy
{
    public static bool HasSameStableIdentity(
        string? inspectedIdentity,
        string? activeIdentity)
        => !string.IsNullOrEmpty(inspectedIdentity) &&
            string.Equals(
                inspectedIdentity,
                activeIdentity,
                StringComparison.Ordinal);

    public static WorldAssetRebindResult<TAsset> Resolve<TAsset>(
        TAsset? currentAsset,
        bool hasStableFileId,
        TAsset? refreshedAsset,
        bool currentIsUntitled,
        bool currentIsDirty)
        where TAsset : class
    {
        if (currentAsset is null || !hasStableFileId)
        {
            return new(currentAsset, currentIsUntitled, null);
        }

        if (refreshedAsset is not null)
        {
            return new(refreshedAsset, currentIsUntitled, currentIsDirty);
        }

        return new(null, true, null);
    }
}
