using SailorEditor.Workspace;

namespace SailorEditor.Content;

public static class ProjectContentPathPolicy
{
    public static StringComparison PathComparison => WorkspacePathPolicy.PathComparison;

    public static StringComparer PathComparer => WorkspacePathPolicy.PathComparer;

    public static string NormalizeRoot(string path)
        => WorkspacePathPolicy.NormalizePhysicalPath(path);

    public static bool IsSamePath(string left, string right)
        => WorkspacePathPolicy.IsSamePath(left, right);

    public static bool IsInsideRoot(string rootPath, string candidatePath)
        => WorkspacePathPolicy.IsInsideRoot(rootPath, candidatePath);
}
