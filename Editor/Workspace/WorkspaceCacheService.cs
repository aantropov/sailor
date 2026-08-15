#nullable enable

namespace SailorEditor.Workspace;

public sealed class WorkspaceCacheService
{
    public void Clear(WorkspaceSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var workspaceRoot = WorkspacePathPolicy.NormalizePhysicalPath(session.WorkspaceRoot);
        var cacheDirectory = WorkspacePathPolicy.NormalizePhysicalPath(session.CacheDirectory);
        WorkspacePathPolicy.EnsureInsideRoot(workspaceRoot, cacheDirectory, nameof(session.CacheDirectory));
        if (WorkspacePathPolicy.IsSamePath(workspaceRoot, cacheDirectory))
        {
            throw new InvalidOperationException(
                "The workspace cache directory cannot be the workspace root.");
        }

        if (Directory.Exists(cacheDirectory))
            Directory.Delete(cacheDirectory, recursive: true);
        Directory.CreateDirectory(cacheDirectory);
    }
}
