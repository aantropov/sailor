#nullable enable

using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace SailorEditor.Workspace;

public sealed record GameBuildCapability(bool CanBuild, string Detail, IReadOnlyList<string> TargetRunner);

public static class GameBuildEnvironment
{
    public static string CMakeExecutable => FindExecutable("cmake") ?? "cmake";

    public static string? FindExecutable(string name)
    {
        if (Path.IsPathRooted(name))
        {
            return File.Exists(name) ? name : null;
        }
        var filename = OperatingSystem.IsWindows() && !name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)
            ? name + ".exe" : name;
        var paths = (Environment.GetEnvironmentVariable("PATH") ?? string.Empty).Split(Path.PathSeparator)
            .Concat(new[] { "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin",
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "CMake", "bin") });
        return paths.Where(path => !string.IsNullOrWhiteSpace(path)).Select(path => Path.Combine(path, filename))
            .FirstOrDefault(File.Exists);
    }

    public static Dictionary<string, string> ReadCMakeCache(string build)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        var file = Path.Combine(build, "CMakeCache.txt");
        if (!File.Exists(file))
        {
            return result;
        }
        foreach (var line in File.ReadLines(file))
        {
            var colon = line.IndexOf(':');
            var equals = line.IndexOf('=');
            if (!line.StartsWith("//", StringComparison.Ordinal) && colon > 0 && equals > colon)
            {
                result[line[..colon]] = line[(equals + 1)..];
            }
        }
        return result;
    }

    public static string FindEngineBuild(string engine, GameBuildPlatform platform, string configuration,
        string toolchainFile = "")
    {
        if (platform == GameBuildSettings.HostPlatform && Directory.Exists(engine))
        {
            foreach (var build in Directory.EnumerateDirectories(engine)
                .Where(path => Path.GetFileName(path).StartsWith("build", StringComparison.OrdinalIgnoreCase))
                .OrderByDescending(path => File.GetLastWriteTimeUtc(Path.Combine(path, "CMakeCache.txt"))))
            {
                var cache = ReadCMakeCache(build);
                if (cache.GetValueOrDefault("CMAKE_HOME_DIRECTORY") is not { } home ||
                    !WorkspacePathPolicy.IsSamePath(home, engine) ||
                    (cache.GetValueOrDefault("CMAKE_BUILD_TYPE") != configuration &&
                        string.IsNullOrEmpty(cache.GetValueOrDefault("CMAKE_CONFIGURATION_TYPES"))))
                {
                    continue;
                }
                var systemFiles = Path.Combine(build, "CMakeFiles");
                var expectedSystem = SystemName(platform);
                if (Directory.Exists(systemFiles) && Directory.EnumerateDirectories(systemFiles).Any(directory =>
                    File.Exists(Path.Combine(directory, "CMakeSystem.cmake")) &&
                    Regex.IsMatch(File.ReadAllText(Path.Combine(directory, "CMakeSystem.cmake")),
                        "set\\(CMAKE_SYSTEM_NAME \"" + expectedSystem + "\"\\)")))
                {
                    if (string.IsNullOrEmpty(toolchainFile) ||
                        cache.GetValueOrDefault("CMAKE_TOOLCHAIN_FILE") is { } configured &&
                        WorkspacePathPolicy.IsSamePath(Path.GetFullPath(configured, engine), Path.GetFullPath(toolchainFile, engine)))
                    {
                        return build;
                    }
                }
            }
        }
        var suffix = string.IsNullOrWhiteSpace(toolchainFile) ? string.Empty : "-" +
            Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(toolchainFile, engine))))[..12];
        return Path.Combine(engine, "Cache", "GameBuild", platform + suffix, configuration);
    }

    public static async Task<GameBuildCapability> CheckAsync(string workspace, GameBuildPlatform platform,
        string toolchainFile, IWorkspaceProcessRunner runner, string? cmake = null,
        CancellationToken cancellationToken = default)
    {
        cmake ??= CMakeExecutable;
        if (FindExecutable(cmake) is null)
        {
            return new(false, "Install CMake to build games.", []);
        }
        var native = string.IsNullOrWhiteSpace(toolchainFile);
        if (native && platform != GameBuildSettings.HostPlatform)
        {
            return new(false, $"Choose a {platform} CMake toolchain to build from this computer.", []);
        }
        if (!native)
        {
            toolchainFile = Path.GetFullPath(toolchainFile, workspace);
            if (!File.Exists(toolchainFile))
            {
                return new(false, "The CMake toolchain file does not exist.", []);
            }
        }
        var signature = native ? "native-" + cmake + "-" + System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture :
            toolchainFile + File.GetLastWriteTimeUtc(toolchainFile).Ticks;
        var identity = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(signature)))[..16];
        var root = Path.Combine(workspace, "Cache", "BuildToolchains", identity);
        var source = Path.Combine(root, "Source");
        var build = Path.Combine(root, "Build");
        Directory.CreateDirectory(source);
        await File.WriteAllTextAsync(Path.Combine(source, "CMakeLists.txt"), """
            cmake_minimum_required(VERSION 3.21)
            project(SailorBuildToolchain LANGUAGES CXX)
            file(WRITE "${CMAKE_BINARY_DIR}/Target.txt"
              "system=${CMAKE_SYSTEM_NAME}\nprocessor=${CMAKE_SYSTEM_PROCESSOR}\nrunner=${CMAKE_CROSSCOMPILING_EMULATOR}\n")
            """, cancellationToken);
        var arguments = new List<string> { "-S", source, "-B", build };
        if (!native)
        {
            arguments.Add("-DCMAKE_TOOLCHAIN_FILE=" + toolchainFile);
        }
        var result = await runner.RunAsync(new(cmake, arguments, workspace), cancellationToken);
        if (!result.Succeeded)
        {
            return new(false, "Toolchain check failed: " + result.Output, []);
        }
        var targetFile = Path.Combine(build, "Target.txt");
        if (!File.Exists(targetFile))
        {
            return new(false, "CMake did not report the target system.", []);
        }
        var fields = File.ReadAllLines(targetFile).Select(line => line.Split('=', 2))
            .Where(pair => pair.Length == 2).ToDictionary(pair => pair[0], pair => pair[1], StringComparer.Ordinal);
        if (fields.GetValueOrDefault("system") != SystemName(platform))
        {
            return new(false, $"This toolchain targets {fields.GetValueOrDefault("system")}, not {platform}.", []);
        }
        var command = (fields.GetValueOrDefault("runner") ?? string.Empty)
            .Split(';', StringSplitOptions.RemoveEmptyEntries);
        if (platform != GameBuildSettings.HostPlatform && command.Length == 0)
        {
            return new(false, "Set CMAKE_CROSSCOMPILING_EMULATOR in the toolchain to prepare assets and validate the target runtime.", []);
        }
        if (command.Length != 0 && FindExecutable(command[0]) is null)
        {
            return new(false, "The target runner is not installed: " + command[0], []);
        }
        return new(true, $"{platform} / {fields.GetValueOrDefault("processor")} — " +
            (native ? "native build tools ready" : "toolchain ready"), command);
    }

    static string SystemName(GameBuildPlatform platform) => platform switch
    {
        GameBuildPlatform.Windows => "Windows",
        GameBuildPlatform.Linux => "Linux",
        _ => "Darwin"
    };
}
