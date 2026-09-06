using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class WorkspaceCacheServiceTests : IDisposable
{
    readonly string root = Directory.CreateTempSubdirectory("sailor-cache-tests-").FullName;
    readonly WorkspaceCacheService service = new();

    WorkspaceSession Session => new(root, Path.Combine(root, "Game.sailor"),
        WorkspaceManifest.CreateDefault("Game", Path.Combine(root, "Engine")),
        Path.Combine(root, "Content"), Path.Combine(root, "Source"),
        Path.Combine(root, "Generated"), Path.Combine(root, "Cache"))
    {
        BuildDirectory = Path.Combine(root, "Cache", "Build"),
        LogicOutputDirectory = Path.Combine(root, "Binaries"),
    };

    [Fact]
    public void Clear_PreservesOldCacheAndProjectFiles()
    {
        var session = Session;
        Directory.CreateDirectory(session.BuildDirectory);
        File.WriteAllText(Path.Combine(session.BuildDirectory, "CMakeCache.txt"), "build cache");
        Directory.CreateDirectory(session.ContentDirectory);
        var scene = Path.Combine(session.ContentDirectory, "Scene.world");
        File.WriteAllText(scene, "scene data");

        var backup = service.Clear(session);

        Assert.NotNull(backup);
        Assert.Empty(Directory.EnumerateFileSystemEntries(session.CacheDirectory));
        Assert.Equal("build cache", File.ReadAllText(Path.Combine(backup, "Build", "CMakeCache.txt")));
        Assert.Equal("scene data", File.ReadAllText(scene));
        Assert.NotEqual(backup, service.Clear(session));
        Assert.True(Directory.Exists(backup));
    }

    [Fact]
    public void Clear_CreatesAMissingCacheWithoutABackup()
    {
        Assert.Null(service.Clear(Session));
        Assert.True(Directory.Exists(Session.CacheDirectory));
    }

    [Theory]
    [InlineData(".")]
    [InlineData("..")]
    [InlineData("Content")]
    [InlineData("Content/Cache")]
    [InlineData("Source")]
    [InlineData("Generated")]
    [InlineData("Binaries")]
    [InlineData("Engine")]
    [InlineData(".sailor")]
    public void Clear_RejectsUnsafeTargets(string path)
    {
        var session = Session with { CacheDirectory = Path.GetFullPath(path, root) };
        Assert.Throws<InvalidOperationException>(() => service.Clear(session));
    }

    [Fact]
    public void Clear_RejectsCacheContainingSource()
    {
        var session = Session with { SourceDirectory = Path.Combine(Session.CacheDirectory, "Source") };
        Assert.Throws<InvalidOperationException>(() => service.Clear(session));
    }

    [Fact]
    public void Clear_DoesNotFollowCacheLinkOutsideWorkspace()
    {
        if (OperatingSystem.IsWindows())
            return; // Creating directory links requires privileges on Windows.

        var workspace = Path.Combine(root, "Workspace");
        var outside = Path.Combine(root, "Outside");
        Directory.CreateDirectory(workspace);
        Directory.CreateDirectory(outside);
        var link = Path.Combine(workspace, "Cache");
        Directory.CreateSymbolicLink(link, outside);
        File.WriteAllText(Path.Combine(outside, "keep.txt"), "keep");
        Assert.Throws<InvalidOperationException>(() => service.Clear(Session with
        {
            WorkspaceRoot = workspace,
            CacheDirectory = link,
        }));
        Assert.Equal("keep", File.ReadAllText(Path.Combine(outside, "keep.txt")));
    }

    public void Dispose() => Directory.Delete(root, recursive: true);
}
