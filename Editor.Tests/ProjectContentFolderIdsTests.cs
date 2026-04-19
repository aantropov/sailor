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
}
