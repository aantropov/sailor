using SailorEditor.Content;

public class ProjectContentPathPolicyTests
{
    [Fact]
    public void IsInsideRoot_RejectsSiblingWithSharedPrefix()
    {
        var root = Path.Combine(Path.GetTempPath(), "sailor-content");
        var sibling = Path.Combine(Path.GetTempPath(), "sailor-content-copy", "asset.asset");

        Assert.False(ProjectContentPathPolicy.IsInsideRoot(root, sibling));
    }

    [Fact]
    public void IsInsideRoot_AcceptsRootAndDescendants()
    {
        var root = Path.Combine(Path.GetTempPath(), "sailor-content");

        Assert.True(ProjectContentPathPolicy.IsInsideRoot(root, root));
        Assert.True(ProjectContentPathPolicy.IsInsideRoot(root, Path.Combine(root, "Materials", "wood.mat.asset")));
    }

    [Fact]
    public void IsInsideRoot_RejectsDescendantSymlinkThatEscapesRoot()
    {
        if (OperatingSystem.IsWindows())
            return;

        var testRoot = Path.Combine(Path.GetTempPath(), $"sailor-path-policy-{Guid.NewGuid():N}");
        var contentRoot = Path.Combine(testRoot, "Content");
        var externalRoot = Path.Combine(testRoot, "External");
        var linkPath = Path.Combine(contentRoot, "ExternalLink");

        try
        {
            Directory.CreateDirectory(contentRoot);
            Directory.CreateDirectory(externalRoot);
            Directory.CreateSymbolicLink(linkPath, externalRoot);

            Assert.False(ProjectContentPathPolicy.IsInsideRoot(contentRoot, Path.Combine(linkPath, "asset.asset")));
        }
        finally
        {
            if (Directory.Exists(testRoot))
                Directory.Delete(testRoot, recursive: true);
        }
    }
}
