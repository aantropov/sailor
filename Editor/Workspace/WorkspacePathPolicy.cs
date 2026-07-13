namespace SailorEditor.Workspace;

public static class WorkspacePathPolicy
{
    public static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;

    public static StringComparer PathComparer => OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase
        : StringComparer.Ordinal;

    public static bool IsSafeRelativePath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return false;

        var normalized = path.Trim().Replace('\\', '/');
        if (normalized[0] == '/' || HasDrivePrefix(normalized))
            return false;

        if (normalized.Split('/', StringSplitOptions.None).Any(x => x == ".."))
            return false;

        try
        {
            var platformPath = normalized.Replace('/', Path.DirectorySeparatorChar);
            if (Path.IsPathRooted(platformPath))
                return false;

            _ = Path.GetFullPath(platformPath, Path.GetFullPath("."));
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static string ResolveOwnedPath(string rootPath, string relativePath, string field)
    {
        if (!IsSafeRelativePath(relativePath))
        {
            throw new InvalidOperationException(
                $"{field} must be a safe relative path inside the workspace root; rooted, drive-relative, and traversal paths can resolve outside it and are not allowed.");
        }

        var root = Path.GetFullPath(rootPath);
        var platformPath = relativePath
            .Replace('/', Path.DirectorySeparatorChar)
            .Replace('\\', Path.DirectorySeparatorChar);
        var candidate = Path.GetFullPath(Path.Combine(root, platformPath));
        EnsureInsideRoot(root, candidate, field);
        return NormalizePhysicalPath(candidate);
    }

    public static void EnsureInsideRoot(string rootPath, string candidatePath, string field)
    {
        var lexicalRoot = NormalizeLexicalPath(rootPath);
        var lexicalCandidate = NormalizeLexicalPath(candidatePath);
        if (!IsContained(lexicalRoot, lexicalCandidate))
            throw new InvalidOperationException($"{field} resolves outside the workspace root.");

        string physicalRoot;
        string physicalCandidate;
        try
        {
            physicalRoot = NormalizePhysicalPath(lexicalRoot);
            physicalCandidate = NormalizePhysicalPath(lexicalCandidate);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            throw new InvalidOperationException($"{field} could not be safely resolved inside the workspace root.", ex);
        }

        if (!IsContained(physicalRoot, physicalCandidate))
        {
            throw new InvalidOperationException(
                $"{field} resolves outside the workspace root through a symbolic link or junction.");
        }
    }

    public static bool IsInsideRoot(string rootPath, string candidatePath)
    {
        try
        {
            var lexicalRoot = NormalizeLexicalPath(rootPath);
            var lexicalCandidate = NormalizeLexicalPath(candidatePath);
            return IsContained(lexicalRoot, lexicalCandidate)
                && IsContained(NormalizePhysicalPath(lexicalRoot), NormalizePhysicalPath(lexicalCandidate));
        }
        catch
        {
            return false;
        }
    }

    public static bool IsSamePath(string left, string right)
    {
        try
        {
            return string.Equals(NormalizePhysicalPath(left), NormalizePhysicalPath(right), PathComparison);
        }
        catch
        {
            return false;
        }
    }

    public static string NormalizePhysicalPath(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var pathRoot = Path.GetPathRoot(fullPath);
        if (string.IsNullOrEmpty(pathRoot))
            return TrimTrailingSeparators(fullPath);

        var current = pathRoot;
        var relativePath = Path.GetRelativePath(pathRoot, fullPath);
        if (relativePath == ".")
            return pathRoot;

        foreach (var part in relativePath.Split(
            [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
            StringSplitOptions.RemoveEmptyEntries))
        {
            var next = Path.Combine(current, part);
            var resolved = ResolveLinkTarget(next);
            current = resolved is null ? next : NormalizePhysicalPath(resolved);
        }

        return TrimTrailingSeparators(current);
    }

    static string NormalizeLexicalPath(string path)
        => TrimTrailingSeparators(Path.GetFullPath(path));

    static string? ResolveLinkTarget(string path)
    {
        var directory = new DirectoryInfo(path);
        if (directory.LinkTarget is not null)
        {
            return directory.ResolveLinkTarget(returnFinalTarget: true)?.FullName
                ?? throw new IOException($"Could not resolve directory link target: {path}");
        }

        if (directory.Exists)
            return null;

        var file = new FileInfo(path);
        if (file.LinkTarget is not null)
        {
            return file.ResolveLinkTarget(returnFinalTarget: true)?.FullName
                ?? throw new IOException($"Could not resolve file link target: {path}");
        }

        return null;
    }

    static bool IsContained(string rootPath, string candidatePath)
    {
        if (string.Equals(rootPath, candidatePath, PathComparison))
            return true;

        var prefix = EndsWithDirectorySeparator(rootPath)
            ? rootPath
            : rootPath + Path.DirectorySeparatorChar;
        return candidatePath.StartsWith(prefix, PathComparison);
    }

    static string TrimTrailingSeparators(string path)
    {
        var pathRoot = Path.GetPathRoot(path);
        return string.Equals(path, pathRoot, PathComparison)
            ? path
            : path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }

    static bool EndsWithDirectorySeparator(string path)
        => path.EndsWith(Path.DirectorySeparatorChar)
            || path.EndsWith(Path.AltDirectorySeparatorChar);

    static bool HasDrivePrefix(string path)
        => path.Length >= 2
            && path[1] == ':'
            && (path[0] is >= 'A' and <= 'Z' || path[0] is >= 'a' and <= 'z');
}
