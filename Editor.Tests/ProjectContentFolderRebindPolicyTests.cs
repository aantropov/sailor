using SailorEditor.Content;

namespace Editor.Tests;

public sealed class ProjectContentFolderRebindPolicyTests : IDisposable
{
    readonly string root = Path.Combine(
        Path.GetTempPath(),
        "sailor-content-folder-rebind",
        Guid.NewGuid().ToString("N"));

    public ProjectContentFolderRebindPolicyTests()
    {
        Directory.CreateDirectory(root);
    }

    [Fact]
    public void MoveOrRename_MapsSelectedDescendantToTheNewPhysicalPath()
    {
        var source = Directory.CreateDirectory(Path.Combine(root, "Models", "Birds")).FullName;
        var selectedDescendant = Directory.CreateDirectory(Path.Combine(source, "Textures", "Nested")).FullName;
        var destination = Directory.CreateDirectory(Path.Combine(root, "Archive")).FullName;
        var target = Path.Combine(destination, "Birds");

        var plan = ProjectContentFolderRebindPolicy.CreatePlan(
            selectedDescendant,
            source,
            target,
            ProjectContentFolderMutationKind.MoveOrRename);

        Assert.True(plan.HasSelection);
        Assert.True(ProjectContentPathPolicy.IsSamePath(plan.OriginalPath!, selectedDescendant));
        Assert.True(ProjectContentPathPolicy.IsSamePath(
            plan.SuccessPath!,
            Path.Combine(target, "Textures", "Nested")));
    }

    [Fact]
    public void MoveOrRename_LeavesAnUnrelatedSelectedFolderAtItsCurrentPath()
    {
        var source = Directory.CreateDirectory(Path.Combine(root, "Models")).FullName;
        var selected = Directory.CreateDirectory(Path.Combine(root, "Materials")).FullName;
        var target = Path.Combine(root, "Archive", "Models");

        var plan = ProjectContentFolderRebindPolicy.CreatePlan(
            selected,
            source,
            target,
            ProjectContentFolderMutationKind.MoveOrRename);

        Assert.True(ProjectContentPathPolicy.IsSamePath(plan.SuccessPath!, selected));
    }

    [Fact]
    public void Delete_FallsBackFromASelectedDescendantToTheDeletedFoldersParent()
    {
        var parent = Directory.CreateDirectory(Path.Combine(root, "Models")).FullName;
        var source = Directory.CreateDirectory(Path.Combine(parent, "Birds")).FullName;
        var selectedDescendant = Directory.CreateDirectory(Path.Combine(source, "Textures")).FullName;

        var plan = ProjectContentFolderRebindPolicy.CreatePlan(
            selectedDescendant,
            source,
            targetFolderPath: null,
            mutationKind: ProjectContentFolderMutationKind.Delete);

        Assert.True(ProjectContentPathPolicy.IsSamePath(plan.SuccessPath!, parent));
    }

    [Fact]
    public void ResolveLiveFolderId_UsesTheNearestLiveParentAndThenTheMountRoot()
    {
        var contentRoot = Directory.CreateDirectory(Path.Combine(root, "Content")).FullName;
        var parent = Directory.CreateDirectory(Path.Combine(contentRoot, "Models")).FullName;
        var missingDescendant = Path.Combine(parent, "Deleted", "Nested");
        var liveFolders = new[]
        {
            new ProjectContentFolderSnapshot(ProjectContentFolderIds.ContentRootId, "Content", -1, contentRoot),
            new ProjectContentFolderSnapshot(42, "Models", ProjectContentFolderIds.ContentRootId, parent)
        };

        Assert.Equal(42, ProjectContentFolderRebindPolicy.ResolveLiveFolderId(missingDescendant, liveFolders));
        Assert.Equal(
            ProjectContentFolderIds.ContentRootId,
            ProjectContentFolderRebindPolicy.ResolveLiveFolderId(Path.Combine(contentRoot, "Deleted"), liveFolders));
    }

    public void Dispose()
    {
        if (Directory.Exists(root))
            Directory.Delete(root, recursive: true);
    }
}
