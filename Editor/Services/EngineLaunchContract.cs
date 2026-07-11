#nullable enable

namespace SailorEditor.Services;

public sealed record EngineLaunchContext(string WorkspaceRoot, string? WorkspaceManifestPath)
{
    public string ContentDirectory => Path.Combine(WorkspaceRoot, "Content");
    public string CacheDirectory => Path.Combine(WorkspaceRoot, "Cache");
    public string TempWorldFilePath => Path.Combine(CacheDirectory, "Temp.world");
    public string EditorTypesCacheFilePath => Path.Combine(CacheDirectory, "EditorTypes.yaml");

    public IReadOnlyList<string> BuildArguments(string world, IEnumerable<string>? extraArguments = null)
    {
        var arguments = new List<string>
        {
            "--workspace",
            WorkspaceRoot
        };

        if (!string.IsNullOrWhiteSpace(WorkspaceManifestPath))
        {
            arguments.Add("--workspace-manifest");
            arguments.Add(WorkspaceManifestPath);
        }

        arguments.Add("--world");
        arguments.Add(world);

        if (extraArguments is not null)
            arguments.AddRange(extraArguments.Where(x => !string.IsNullOrWhiteSpace(x)));

        return arguments;
    }

    public IReadOnlyList<string> BuildInteropArguments(
        string executableToken,
        string world,
        IEnumerable<string>? extraArguments = null)
        => new[] { executableToken }
            .Concat(BuildArguments(world, extraArguments))
            .ToArray();
}

public static class EngineLaunchContract
{
    public static EngineLaunchContext Resolve(string? activeWorkspaceRoot, string fallbackRoot)
        => Resolve(activeWorkspaceRoot, null, fallbackRoot);

    public static EngineLaunchContext Resolve(
        string? activeWorkspaceRoot,
        string? activeWorkspaceManifestPath,
        string fallbackRoot)
    {
        var selectedRoot = string.IsNullOrWhiteSpace(activeWorkspaceRoot)
            ? fallbackRoot
            : activeWorkspaceRoot;
        var manifestPath = string.IsNullOrWhiteSpace(activeWorkspaceRoot) ||
            string.IsNullOrWhiteSpace(activeWorkspaceManifestPath)
            ? null
            : NormalizePath(activeWorkspaceManifestPath);

        return new EngineLaunchContext(NormalizeRoot(selectedRoot), manifestPath);
    }

    static string NormalizeRoot(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var pathRoot = Path.GetPathRoot(fullPath);
        return string.Equals(fullPath, pathRoot, PathComparison)
            ? fullPath
            : fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }

    static string NormalizePath(string path) => Path.GetFullPath(path);

    static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}
