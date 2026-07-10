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
            new ProjectContentFolderSnapshot(3, "Wood", 1),
            new ProjectContentFolderSnapshot(4, "Metal", 1),
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

        Assert.Equal(["Metal", "Wood"], projection.VisibleFolders.Select(x => x.Name).ToArray());
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
        Assert.Equal(["Textures"], projection.VisibleFolders.Select(x => x.Name).ToArray());
        Assert.Single(projection.CurrentFolderAssets);
    }

    [Fact]
    public void Build_UsesActiveWorkspaceRootForFolderPaths()
    {
        var repoRoot = Path.Combine(Path.DirectorySeparatorChar.ToString(), "repo", "Content");
        var workspaceRoot = Path.Combine(Path.DirectorySeparatorChar.ToString(), "workspaces", "Sandbox", "Content");
        var folders = new[]
        {
            new ProjectContentFolderSnapshot(1, "Materials", -1),
            new ProjectContentFolderSnapshot(2, "Wood", 1),
        };

        var repoProjection = ProjectContentProjectionBuilder.Build(
            repoRoot,
            folders,
            [new ProjectContentAssetSnapshot("{R}", "repo-material", ".mat", 1, Path.Combine(repoRoot, "Materials", "repo-material.mat"))],
            new ProjectContentState(CurrentFolderId: 1));

        var workspaceProjection = ProjectContentProjectionBuilder.Build(
            workspaceRoot,
            folders,
            [new ProjectContentAssetSnapshot("{W}", "workspace-material", ".mat", 1, Path.Combine(workspaceRoot, "Materials", "workspace-material.mat"))],
            new ProjectContentState(CurrentFolderId: 1));

        Assert.Equal(Path.Combine(repoRoot, "Materials"), repoProjection.Folders.Single(x => x.FolderId == 1).FullPath);
        Assert.Equal(Path.Combine(workspaceRoot, "Materials"), workspaceProjection.Folders.Single(x => x.FolderId == 1).FullPath);
        Assert.Equal(["repo-material"], repoProjection.VisibleAssets.Select(x => x.DisplayName).ToArray());
        Assert.Equal(["workspace-material"], workspaceProjection.VisibleAssets.Select(x => x.DisplayName).ToArray());
    }
}
