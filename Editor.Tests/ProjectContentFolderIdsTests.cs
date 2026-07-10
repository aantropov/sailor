using SailorEditor.Content;

public class ProjectContentFolderIdsTests
{
    [Theory]
    [InlineData("Materials", "Materials")]
    [InlineData("Materials/Wood", "Materials\\Wood")]
    [InlineData("materials/wood", "Materials/Wood")]
    public void FromRelativePath_IsStableAcrossPathFormatting(string left, string right)
    {
        Assert.Equal(ProjectContentFolderIds.FromRelativePath(left), ProjectContentFolderIds.FromRelativePath(right));
    }

    [Fact]
    public void FromRelativePath_ProducesDistinctIdsForDifferentFolders()
    {
        var materials = ProjectContentFolderIds.FromRelativePath("Materials");
        var models = ProjectContentFolderIds.FromRelativePath("Models");
        var nested = ProjectContentFolderIds.FromRelativePath("Materials/Wood");

        Assert.NotEqual(materials, models);
        Assert.NotEqual(materials, nested);
        Assert.NotEqual(models, nested);
    }

    [Fact]
    public void FromRootedRelativePath_IsDistinctAcrossContentRoots()
    {
        var workspaceMaterials = ProjectContentFolderIds.FromRootedRelativePath(1, "Materials");
        var engineMaterials = ProjectContentFolderIds.FromRootedRelativePath(2, "Materials");

        Assert.NotEqual(workspaceMaterials, engineMaterials);
        Assert.NotEqual(ProjectContentFolderIds.WorkspaceContentRootId, workspaceMaterials);
        Assert.NotEqual(ProjectContentFolderIds.EngineContentRootId, engineMaterials);
    }

    [Fact]
    public void Allocator_ProbesPastAnExistingFolderId()
    {
        var allocator = new ProjectContentFolderIdAllocator();
        var original = ProjectContentFolderIds.FromRootedRelativePath(1, "Materials");
        allocator.Reserve(original);

        var allocated = allocator.Allocate(1, "Materials", useRootedFolderIds: true);

        Assert.NotEqual(original, allocated);
        Assert.NotEqual(ProjectContentFolderIds.WorkspaceContentRootId, allocated);
        Assert.NotEqual(ProjectContentFolderIds.EngineContentRootId, allocated);
    }
}
