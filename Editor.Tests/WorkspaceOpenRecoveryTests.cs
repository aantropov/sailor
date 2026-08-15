using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class WorkspaceOpenRecoveryTests
{
    [Fact]
    public async Task PendingMarkerLivesInProjectCacheAndSurvivesUntilCleared()
    {
        var root = Path.Combine(Path.GetTempPath(), $"SailorRecovery-{Guid.NewGuid():N}");
        var session = CreateSession(root);
        var recovery = new WorkspaceOpenRecoveryService();
        try
        {
            await recovery.MarkPendingAsync(session);

            var markerPath = recovery.GetPendingMarkerPath(session);
            Assert.StartsWith(session.CacheDirectory, markerPath, StringComparison.Ordinal);
            Assert.True(File.Exists(markerPath));
            Assert.True(recovery.Assess(session).PreviousOpenWasInterrupted);

            recovery.ClearPending(session);

            Assert.False(File.Exists(markerPath));
            Assert.False(recovery.Assess(session).PreviousOpenWasInterrupted);
        }
        finally
        {
            if (Directory.Exists(root))
                Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void MissingModuleBinaryRequiresBuild()
    {
        var root = Path.Combine(Path.GetTempPath(), $"SailorRecovery-{Guid.NewGuid():N}");
        var session = CreateSession(root);
        var recovery = new WorkspaceOpenRecoveryService();
        try
        {
            var missing = recovery.Assess(session);

            Assert.True(missing.ModuleBinaryIsMissing);
            Assert.True(missing.RequiresBuild);

            Directory.CreateDirectory(Path.GetDirectoryName(missing.ModuleBinaryPath)!);
            File.WriteAllText(missing.ModuleBinaryPath, "module");

            var available = recovery.Assess(session);
            Assert.False(available.ModuleBinaryIsMissing);
            Assert.False(available.RequiresBuild);
        }
        finally
        {
            if (Directory.Exists(root))
                Directory.Delete(root, recursive: true);
        }
    }

    static WorkspaceSession CreateSession(string root)
    {
        var manifest = WorkspaceManifest.CreateDefault("Game", root) with
        {
            LogicModuleName = "RecoveryGame"
        };
        return new WorkspaceSession(
            root,
            Path.Combine(root, "workspace.sailor"),
            manifest,
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            Path.Combine(root, "Cache"))
        {
            BuildDirectory = Path.Combine(root, "Cache", "Build"),
            LogicOutputDirectory = Path.Combine(root, "Binaries")
        };
    }
}
