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
        Assert.True(projection.IsRootVisible);
        Assert.Equal(1, projection.TopLevelDepth);
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

    [Fact]
    public void Build_UsesFolderSnapshotFullPathForMultipleRoots()
    {
        var workspaceRoot = Path.Combine(Path.DirectorySeparatorChar.ToString(), "workspaces", "Sandbox", "Content");
        var engineRoot = Path.Combine(Path.DirectorySeparatorChar.ToString(), "repo", "Content");
        var workspaceMaterialsId = ProjectContentFolderIds.FromRootedRelativePath(1, "Materials");
        var engineMaterialsId = ProjectContentFolderIds.FromRootedRelativePath(2, "Materials");
        var folders = new[]
        {
            new ProjectContentFolderSnapshot(ProjectContentFolderIds.ContentRootId, "Content", -1, workspaceRoot),
            new ProjectContentFolderSnapshot(workspaceMaterialsId, "Materials", ProjectContentFolderIds.ContentRootId, Path.Combine(workspaceRoot, "Materials")),
            new ProjectContentFolderSnapshot(ProjectContentFolderIds.EngineContentRootId, "Engine Content", -1, engineRoot, IsReadOnly: true),
            new ProjectContentFolderSnapshot(engineMaterialsId, "Materials", ProjectContentFolderIds.EngineContentRootId, Path.Combine(engineRoot, "Materials"), IsReadOnly: true),
        };

        var projection = ProjectContentProjectionBuilder.Build(
            workspaceRoot,
            folders,
            [],
            new ProjectContentState());

        Assert.Equal(
            [Path.Combine(engineRoot, "Materials"), Path.Combine(workspaceRoot, "Materials")],
            projection.Folders.Where(x => x.Name == "Materials").Select(x => x.FullPath).Order(StringComparer.OrdinalIgnoreCase).ToArray());
        Assert.False(projection.IsRootVisible);
        Assert.Equal(0, projection.TopLevelDepth);
        Assert.Equal(["Content", "Engine Content"], projection.VisibleFolders.Select(x => x.Name).ToArray());
        Assert.True(projection.Folders.Single(x => x.FolderId == ProjectContentFolderIds.EngineContentRootId).IsReadOnly);
        Assert.False(projection.Folders.Single(x => x.FolderId == ProjectContentFolderIds.ContentRootId).IsReadOnly);
    }

    [Fact]
    public void Build_ShowsEngineMountAtTopLevelWithoutWorkspace()
    {
        var engineRoot = Path.Combine(Path.DirectorySeparatorChar.ToString(), "repo", "Content");
        var engineMaterialsId = ProjectContentFolderIds.FromRootedRelativePath(2, "Materials");
        var projection = ProjectContentProjectionBuilder.Build(
            engineRoot,
            [
                new ProjectContentFolderSnapshot(ProjectContentFolderIds.EngineContentRootId, "Engine Content", -1, engineRoot),
                new ProjectContentFolderSnapshot(engineMaterialsId, "Materials", ProjectContentFolderIds.EngineContentRootId, Path.Combine(engineRoot, "Materials"))
            ],
            [],
            new ProjectContentState());

        Assert.False(projection.IsRootVisible);
        Assert.Equal(0, projection.TopLevelDepth);
        Assert.Equal(["Engine Content"], projection.VisibleFolders.Select(x => x.Name).ToArray());
        Assert.DoesNotContain(projection.Folders, x => x.Name == "Content");
    }

    [Fact]
    public void Build_KeepsDuplicateFileIdsDistinctByMountedPath()
    {
        var workspacePath = Path.Combine(Path.GetTempPath(), "workspace", "Content", "Materials", "wood.mat");
        var enginePath = Path.Combine(Path.GetTempPath(), "engine", "Content", "Materials", "wood.mat");
        var assets = new[]
        {
            new ProjectContentAssetSnapshot("{WOOD}", "wood", ".mat", 1, workspacePath, ProjectRootId: 1),
            new ProjectContentAssetSnapshot("{WOOD}", "wood", ".mat", 2, enginePath, ProjectRootId: 2, IsReadOnly: true),
        };

        var projection = ProjectContentProjectionBuilder.Build(
            Path.Combine(Path.GetTempPath(), "workspace", "Content"),
            [new ProjectContentFolderSnapshot(2, "Engine Materials", -1, Path.GetDirectoryName(enginePath), IsReadOnly: true)],
            assets,
            new ProjectContentState(CurrentFolderId: 2, SelectedAssetFileId: "{WOOD}", SelectedAssetPath: enginePath));

        Assert.Single(projection.VisibleAssets);
        Assert.Equal(enginePath, projection.VisibleAssets[0].FullPath);
        Assert.True(projection.VisibleAssets[0].IsReadOnly);
        Assert.Equal(enginePath, projection.State.SelectedAssetPath);
    }

    [Fact]
    public void Build_ClearsSelectionThatIsMissingAfterRootSwitch()
    {
        var projection = ProjectContentProjectionBuilder.Build(
            Path.Combine(Path.GetTempPath(), "workspace-b", "Content"),
            [],
            [],
            new ProjectContentState(SelectedAssetFileId: "{OLD}", SelectedAssetPath: Path.Combine(Path.GetTempPath(), "workspace-a", "Content", "old.mat")));

        Assert.Null(projection.State.SelectedAssetFileId);
        Assert.Null(projection.State.SelectedAssetPath);
    }
}
