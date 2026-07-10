namespace SailorEditor.Content;

public static class ProjectContentPathPolicy
{
    public static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;

    public static StringComparer PathComparer => OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;

    public static string NormalizeRoot(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var pathRoot = Path.GetPathRoot(fullPath);
        if (string.IsNullOrEmpty(pathRoot))
            return TrimTrailingSeparators(fullPath);

        var current = pathRoot;
        var relativePath = Path.GetRelativePath(pathRoot, fullPath);
        if (relativePath == ".")
            return pathRoot;

        foreach (var part in relativePath.Split([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar], StringSplitOptions.RemoveEmptyEntries))
        {
            var next = Path.Combine(current, part);
            FileSystemInfo? info = Directory.Exists(next)
                ? new DirectoryInfo(next)
                : File.Exists(next) ? new FileInfo(next) : null;
            var resolved = info?.LinkTarget is not null
                ? info.ResolveLinkTarget(returnFinalTarget: true)
                : null;
            current = resolved?.FullName ?? next;
        }

        return TrimTrailingSeparators(current);
    }

    public static bool IsSamePath(string left, string right)
        => string.Equals(NormalizeRoot(left), NormalizeRoot(right), PathComparison);

    public static bool IsInsideRoot(string rootPath, string candidatePath)
    {
        var root = NormalizeRoot(rootPath);
        var candidate = NormalizeRoot(candidatePath);

        return string.Equals(root, candidate, PathComparison)
            || candidate.StartsWith(root + Path.DirectorySeparatorChar, PathComparison);
    }

    static string TrimTrailingSeparators(string path)
    {
        var pathRoot = Path.GetPathRoot(path);
        return string.Equals(path, pathRoot, PathComparison)
            ? path
            : path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }
}
