using SailorEditor.Services;

namespace Editor.Tests;

public sealed class WorldAssetRebindPolicyTests
{
    [Fact]
    public void Resolve_RebindsByStableIdentityAndPreservesDirtyState()
    {
        var current = new object();
        var refreshed = new object();

        var result = WorldAssetRebindPolicy.Resolve(
            current,
            hasStableFileId: true,
            refreshed,
            currentIsUntitled: false,
            currentIsDirty: true);

        Assert.Same(refreshed, result.Asset);
        Assert.False(result.IsUntitled);
        Assert.True(result.DirtyState);
    }

    [Fact]
    public void Resolve_MarksMissingWorldUntitled()
    {
        var result = WorldAssetRebindPolicy.Resolve(
            new object(),
            hasStableFileId: true,
            refreshedAsset: null,
            currentIsUntitled: false,
            currentIsDirty: true);

        Assert.Null(result.Asset);
        Assert.True(result.IsUntitled);
        Assert.Null(result.DirtyState);
    }

    [Fact]
    public void Resolve_LeavesUntitledOrUnidentifiedWorldAlone()
    {
        var current = new object();

        var result = WorldAssetRebindPolicy.Resolve(
            current,
            hasStableFileId: false,
            refreshedAsset: new object(),
            currentIsUntitled: true,
            currentIsDirty: false);

        Assert.Same(current, result.Asset);
        Assert.True(result.IsUntitled);
        Assert.Null(result.DirtyState);
    }
}
