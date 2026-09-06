#nullable enable

namespace SailorEditor.Workspace;

public sealed class WorkspaceCacheService
{
    // Keep the old cache beside the fresh one so clearing is recoverable and
    // never recursively follows user-provided paths or links.
    public string? Clear(WorkspaceSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var root = WorkspacePathPolicy.NormalizePhysicalPath(session.WorkspaceRoot);
        var cache = WorkspacePathPolicy.NormalizePhysicalPath(session.CacheDirectory);
        WorkspacePathPolicy.EnsureInsideRoot(root, cache, nameof(session.CacheDirectory));
        if (WorkspacePathPolicy.IsSamePath(root, cache))
            throw new InvalidOperationException("The workspace root cannot be cleared as a cache.");

        var enginePath = session.Manifest.EnginePath
            .Replace('/', Path.DirectorySeparatorChar)
            .Replace('\\', Path.DirectorySeparatorChar);
        var protectedPaths = new[]
        {
            session.ManifestPath,
            session.ContentDirectory,
            session.SourceDirectory,
            session.GeneratedProjectDirectory,
            session.LogicOutputDirectory,
            Path.Combine(root, ".sailor"),
            Path.GetFullPath(enginePath, root),
        };
        foreach (var path in protectedPaths.Where(path => !string.IsNullOrWhiteSpace(path)))
        {
            var protectedPath = WorkspacePathPolicy.NormalizePhysicalPath(path);
            if (WorkspacePathPolicy.IsInsideRoot(cache, protectedPath) ||
                WorkspacePathPolicy.IsInsideRoot(protectedPath, cache))
            {
                throw new InvalidOperationException(
                    $"The cache overlaps a protected project or engine path: '{path}'.");
            }
        }

        string? backupPath = null;
        if (Directory.Exists(cache))
        {
            backupPath = cache + ".cleared-" + Guid.NewGuid().ToString("N");
            Directory.Move(cache, backupPath);
        }
        try
        {
            Directory.CreateDirectory(cache);
        }
        catch
        {
            if (backupPath is not null && !Directory.Exists(cache))
                Directory.Move(backupPath, cache);
            throw;
        }
        return backupPath;
    }
}
