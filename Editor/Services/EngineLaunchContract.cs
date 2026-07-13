#nullable enable

using SailorEditor.Workspace;

namespace SailorEditor.Services;

public sealed record EngineLaunchContext(
    string WorkspaceRoot,
    string? WorkspaceManifestPath,
    string ContentDirectory,
    string CacheDirectory,
    string WorkspaceIdentity)
{
    public string TempWorldFilePath => Path.Combine(CacheDirectory, "Temp.world");
    public string TempWorldRuntimePath => Path.GetRelativePath(ContentDirectory, TempWorldFilePath);
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
    public static EngineLaunchContext Resolve(
        string? activeWorkspaceRoot,
        string? activeWorkspaceManifestPath,
        string? activeContentDirectory,
        string? activeCacheDirectory,
        string fallbackRoot,
        string? activeWorkspaceIdentity = null)
    {
        var hasActiveWorkspace = !string.IsNullOrWhiteSpace(activeWorkspaceRoot);
        var selectedRoot = hasActiveWorkspace ? activeWorkspaceRoot : fallbackRoot;
        var selectedContentDirectory = hasActiveWorkspace
            ? RequireResolvedPath(activeContentDirectory, nameof(activeContentDirectory))
            : Path.Combine(fallbackRoot, "Content");
        var selectedCacheDirectory = hasActiveWorkspace
            ? RequireResolvedPath(activeCacheDirectory, nameof(activeCacheDirectory))
            : Path.Combine(fallbackRoot, "Cache");
        var manifestPath = hasActiveWorkspace && !string.IsNullOrWhiteSpace(activeWorkspaceManifestPath)
            ? NormalizePath(activeWorkspaceManifestPath)
            : null;

        return new EngineLaunchContext(
            NormalizeRoot(selectedRoot!),
            manifestPath,
            NormalizeRoot(selectedContentDirectory),
            NormalizeRoot(selectedCacheDirectory),
            ResolveWorkspaceIdentity(hasActiveWorkspace ? activeWorkspaceIdentity : null, selectedRoot!));
    }

    static string ResolveWorkspaceIdentity(string? workspaceIdentity, string workspaceRoot)
    {
        if (!string.IsNullOrWhiteSpace(workspaceIdentity))
            return workspaceIdentity.Trim();

        var normalizedRoot = WorkspacePathPolicy.NormalizePhysicalPath(workspaceRoot)
            .Replace(Path.DirectorySeparatorChar, '/');
        if (Path.AltDirectorySeparatorChar != Path.DirectorySeparatorChar)
            normalizedRoot = normalizedRoot.Replace(Path.AltDirectorySeparatorChar, '/');
        if (OperatingSystem.IsWindows())
            normalizedRoot = FoldAsciiCase(normalizedRoot);

        return $"legacy-root:{normalizedRoot}";
    }

    static string FoldAsciiCase(string value)
    {
        var characters = value.ToCharArray();
        for (var index = 0; index < characters.Length; ++index)
        {
            if (characters[index] is >= 'A' and <= 'Z')
                characters[index] = (char)(characters[index] + ('a' - 'A'));
        }

        return new string(characters);
    }

    static string RequireResolvedPath(string? path, string parameterName)
        => string.IsNullOrWhiteSpace(path)
            ? throw new ArgumentException("An active workspace must provide its resolved content and cache paths.", parameterName)
            : path;

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
