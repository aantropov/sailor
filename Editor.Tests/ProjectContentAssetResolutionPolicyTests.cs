using SailorEditor.Content;

public class ProjectContentAssetResolutionPolicyTests
{
    [Fact]
    public void WorkspaceAssetOverridesEngineAsset()
    {
        Assert.True(ProjectContentAssetResolutionPolicy.ShouldReplace(
            existingIsReadOnly: true,
            incomingIsReadOnly: false));
    }

    [Theory]
    [InlineData(false, true)]
    [InlineData(false, false)]
    [InlineData(true, true)]
    public void EngineOrSameMountDuplicateDoesNotReplaceExisting(bool existingIsReadOnly, bool incomingIsReadOnly)
    {
        Assert.False(ProjectContentAssetResolutionPolicy.ShouldReplace(existingIsReadOnly, incomingIsReadOnly));
    }
}
