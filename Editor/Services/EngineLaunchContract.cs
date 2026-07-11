#nullable enable

namespace SailorEditor.Services;

public sealed record EngineLaunchContext(string WorkspaceRoot)
{
    public string ContentDirectory => Path.Combine(WorkspaceRoot, "Content");
    public string CacheDirectory => Path.Combine(WorkspaceRoot, "Cache");
    public string TempWorldFilePath => Path.Combine(CacheDirectory, "Temp.world");
    public string EngineTypesCacheFilePath => Path.Combine(CacheDirectory, "EngineTypes.yaml");

    public IReadOnlyList<string> BuildArguments(string world, IEnumerable<string>? extraArguments = null)
    {
        var arguments = new List<string>
        {
            "--workspace",
            WorkspaceRoot,
            "--world",
            world
        };

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
    {
        var selectedRoot = string.IsNullOrWhiteSpace(activeWorkspaceRoot)
            ? fallbackRoot
            : activeWorkspaceRoot;

        return new EngineLaunchContext(NormalizeRoot(selectedRoot));
    }

    static string NormalizeRoot(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var pathRoot = Path.GetPathRoot(fullPath);
        return string.Equals(fullPath, pathRoot, PathComparison)
            ? fullPath
            : fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
    }

    static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}
