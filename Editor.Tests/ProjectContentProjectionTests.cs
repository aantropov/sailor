using SailorEditor.Content;

namespace SailorEditor.Editor.Tests;

public class ProjectContentProjectionTests
{
    [Fact]
    public void Build_FiltersAndSortsAssetsInsideCurrentFolder()
    {
        var folders = new[]
        {
            new ProjectContentFolderSnapshot(1, "Materials", -1),
            new ProjectContentFolderSnapshot(2, "Models", -1),
        };

        var assets = new[]
        {
            new ProjectContentAssetSnapshot("{B}", "wood", ".mat", 1, "/Content/Materials/wood.mat"),
            new ProjectContentAssetSnapshot("{A}", "metal", ".mat", 1, "/Content/Materials/metal.mat"),
            new ProjectContentAssetSnapshot("{C}", "duck", ".glb", 2, "/Content/Models/duck.glb"),
        };

        var projection = ProjectContentProjectionBuilder.Build(
            "/Content",
            folders,
            assets,
            new ProjectContentState(CurrentFolderId: 1, Filter: "m", SortMode: ProjectContentSortMode.NameDescending));

        Assert.Equal(["wood", "metal"], projection.VisibleAssets.Select(x => x.DisplayName).ToArray());
        Assert.Equal(1, projection.State.CurrentFolderId);
        Assert.Equal("/Content/Materials", projection.Folders.Single(x => x.FolderId == 1).FullPath);
    }

    [Fact]
    public void Build_ResetsMissingFolderSelectionToRoot()
    {
        var projection = ProjectContentProjectionBuilder.Build(
            "/Content",
            [new ProjectContentFolderSnapshot(1, "Textures", -1)],
            [new ProjectContentAssetSnapshot("{A}", "albedo", ".png", -1, "/Content/albedo.png")],
            new ProjectContentState(CurrentFolderId: 99));

        Assert.Null(projection.State.CurrentFolderId);
        Assert.Single(projection.CurrentFolderAssets);
    }
}
