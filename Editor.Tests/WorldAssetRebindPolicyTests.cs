using SailorEditor.Services;

namespace Editor.Tests;

public sealed class WorldAssetRebindPolicyTests
{
    [Fact]
    public void HasSameStableIdentity_AcceptsRefreshedIdentityInstance()
    {
        var inspectedIdentity = new string("{15500000-0000-4000-8000-000000000155}".ToCharArray());
        var refreshedIdentity = new string("{15500000-0000-4000-8000-000000000155}".ToCharArray());

        Assert.NotSame(inspectedIdentity, refreshedIdentity);
        Assert.True(WorldAssetRebindPolicy.HasSameStableIdentity(
            inspectedIdentity,
            refreshedIdentity));
        Assert.False(WorldAssetRebindPolicy.HasSameStableIdentity(
            inspectedIdentity,
            "{15500000-0000-4000-8000-000000000156}"));
        Assert.False(WorldAssetRebindPolicy.HasSameStableIdentity(string.Empty, string.Empty));
    }

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
