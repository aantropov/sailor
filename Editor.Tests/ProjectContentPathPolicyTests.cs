using System.Diagnostics;
using SailorEditor.Content;
using SailorEditor.Workspace;

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
        var testRoot = Path.Combine(Path.GetTempPath(), $"sailor-path-policy-{Guid.NewGuid():N}");
        var contentRoot = Path.Combine(testRoot, "Content");
        var externalRoot = Path.Combine(testRoot, "External");
        var linkPath = Path.Combine(contentRoot, "ExternalLink");

        try
        {
            Directory.CreateDirectory(contentRoot);
            Directory.CreateDirectory(externalRoot);
            CreateDirectoryLink(linkPath, externalRoot);

            Assert.False(ProjectContentPathPolicy.IsInsideRoot(contentRoot, Path.Combine(linkPath, "asset.asset")));
            Assert.False(WorkspacePathPolicy.IsInsideRoot(contentRoot, Path.Combine(linkPath, "asset.asset")));
        }
        finally
        {
            if (Directory.Exists(linkPath))
                Directory.Delete(linkPath);
            if (Directory.Exists(testRoot))
                Directory.Delete(testRoot, recursive: true);
        }
    }

    [Fact]
    public void IsSamePath_ResolvesSymlinkTargetAliases()
    {
        var testRoot = Path.Combine(Path.GetTempPath(), $"sailor-path-alias-{Guid.NewGuid():N}");
        var contentRoot = Path.Combine(testRoot, "Content");
        var linkPath = Path.Combine(contentRoot, "Cycle");

        try
        {
            Directory.CreateDirectory(contentRoot);
            CreateDirectoryLink(linkPath, contentRoot);

            Assert.Equal(
                WorkspacePathPolicy.NormalizePhysicalPath(contentRoot),
                WorkspacePathPolicy.NormalizePhysicalPath(linkPath),
                ignoreCase: OperatingSystem.IsWindows());
            Assert.True(ProjectContentPathPolicy.IsSamePath(contentRoot, linkPath));
        }
        finally
        {
            if (Directory.Exists(linkPath))
                Directory.Delete(linkPath);
            if (Directory.Exists(testRoot))
                Directory.Delete(testRoot, recursive: true);
        }
    }

    static void CreateDirectoryLink(string linkPath, string targetPath)
    {
        if (!OperatingSystem.IsWindows())
        {
            Directory.CreateSymbolicLink(linkPath, targetPath);
            return;
        }

        var startInfo = new ProcessStartInfo("cmd.exe")
        {
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("/d");
        startInfo.ArgumentList.Add("/c");
        startInfo.ArgumentList.Add("mklink");
        startInfo.ArgumentList.Add("/J");
        startInfo.ArgumentList.Add(linkPath);
        startInfo.ArgumentList.Add(targetPath);

        using var process = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start mklink.");
        process.WaitForExit();
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"Could not create test junction: {process.StandardError.ReadToEnd()}{process.StandardOutput.ReadToEnd()}");
        }
    }
}
