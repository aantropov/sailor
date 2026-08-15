using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class WorkspaceCacheServiceTests
{
    [Fact]
    public void Clear_DeletesOnlyConfiguredProjectCacheAndRecreatesIt()
    {
        var root = Path.Combine(Path.GetTempPath(), $"SailorCache-{Guid.NewGuid():N}");
        var session = CreateSession(root, Path.Combine(root, "Cache"));
        var cacheFile = Path.Combine(session.CacheDirectory, "nested", "cached.bin");
        var contentFile = Path.Combine(session.ContentDirectory, "level.world");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(cacheFile)!);
            Directory.CreateDirectory(session.ContentDirectory);
            File.WriteAllText(cacheFile, "cache");
            File.WriteAllText(contentFile, "content");

            new WorkspaceCacheService().Clear(session);

            Assert.True(Directory.Exists(session.CacheDirectory));
            Assert.False(File.Exists(cacheFile));
            Assert.True(File.Exists(contentFile));
        }
        finally
        {
            if (Directory.Exists(root))
                Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void Clear_RejectsWorkspaceRootAsCacheDirectory()
    {
        var root = Path.Combine(Path.GetTempPath(), $"SailorCache-{Guid.NewGuid():N}");
        var session = CreateSession(root, root);

        Assert.Throws<InvalidOperationException>(() =>
            new WorkspaceCacheService().Clear(session));
    }

    static WorkspaceSession CreateSession(string root, string cacheDirectory)
    {
        return new WorkspaceSession(
            root,
            Path.Combine(root, "workspace.sailor"),
            WorkspaceManifest.CreateDefault("Game", root),
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            cacheDirectory)
        {
            BuildDirectory = Path.Combine(cacheDirectory, "Build"),
            LogicOutputDirectory = Path.Combine(root, "Binaries")
        };
    }
}
