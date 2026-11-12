#nullable enable

using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace SailorEditor.Workspace;

public sealed record GameBuildRequest(
    string WorkspaceDirectory,
    string EngineDirectory,
    string OutputDirectory,
    string Configuration,
    GameBuildPlatform Platform,
    GamePlatformBuildSettings Options)
{
    public string? EngineBuildDirectory { get; init; }
    public string CMakeExecutable { get; init; } = "cmake";
    public bool BuildRuntime { get; init; } = true;
}

public sealed record GameBuildResult(bool Succeeded, string OutputDirectory, string? Error = null);

public sealed class GameBuildService(IWorkspaceProcessRunner runner)
{
    readonly SemaphoreSlim _gate = new(1, 1);
    static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow
    };

    sealed record WorkspaceDescription
    {
        public required int SchemaVersion { get; init; }
        public required bool Legacy { get; init; }
        public required bool EngineMode { get; init; }
        public required string WorkspaceId { get; init; }
        public required string Name { get; init; }
        public required string Content { get; init; }
        public required string EngineContent { get; init; }
        public required string EngineRoot { get; init; }
        public required string GeneratedProject { get; init; }
        public required string Cache { get; init; }
        public required string LogicOutput { get; init; }
        public required string LogicModuleName { get; init; }
    }

    public async Task<GameBuildResult> BuildAsync(GameBuildRequest request,
        IProgress<string>? progress = null, CancellationToken cancellationToken = default)
    {
        await _gate.WaitAsync(cancellationToken);
        string? staging = null;
        try
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(request.WorkspaceDirectory);
            ArgumentException.ThrowIfNullOrWhiteSpace(request.EngineDirectory);
            if (request.Platform != GameBuildSettings.HostPlatform)
            {
                throw new InvalidOperationException($"Build {request.Platform} on a native {request.Platform} host.");
            }
            new GameBuildSettings { Configuration = request.Configuration,
                Platforms = new GameBuildSettings().Platforms.ToDictionary(pair => pair.Key,
                    pair => pair.Key == request.Platform ? request.Options : pair.Value) }.Validate();
            var workspace = WorkspacePathPolicy.NormalizePhysicalPath(request.WorkspaceDirectory);
            var engine = WorkspacePathPolicy.NormalizePhysicalPath(request.EngineDirectory);
            if (string.IsNullOrWhiteSpace(request.OutputDirectory))
            {
                throw new ArgumentException("Choose a new output directory.");
            }
            var output = WorkspacePathPolicy.NormalizePhysicalPath(Path.GetFullPath(request.OutputDirectory, workspace));
            if (File.Exists(output) || Directory.Exists(output) || new DirectoryInfo(output).LinkTarget is not null)
            {
                throw new IOException("The output must be a new directory; existing builds are preserved.");
            }
            if (WorkspacePathPolicy.IsInsideRoot(workspace, output) || WorkspacePathPolicy.IsInsideRoot(engine, output))
            {
                throw new IOException("The output must be outside the workspace and engine directories.");
            }
            if (!Directory.Exists(workspace) || !Directory.Exists(engine))
            {
                throw new DirectoryNotFoundException("Workspace and engine directories must exist.");
            }
            if (File.Exists(Path.Combine(workspace, "GamePackage.json")))
            {
                throw new InvalidOperationException("Select an authoring workspace to export; this folder is already a game package.");
            }
            var sourceEngine = File.Exists(Path.Combine(engine, "CMakeLists.txt")) && Directory.Exists(Path.Combine(engine, "Runtime"));
            var build = Path.GetFullPath(request.EngineBuildDirectory ??
                Path.Combine(engine, "Cache", "GameBuild", request.Platform.ToString(), request.Configuration), engine);
            var sdk = sourceEngine ? Path.Combine(build, "GameSDK", request.Configuration) : engine;
            var suffix = request.Platform == GameBuildPlatform.Windows ? ".exe" : "";
            var executableName = "SailorEngine-" + request.Configuration + suffix;
            if (sourceEngine)
            {
                var cache = ReadCMakeCache(build);
                if (cache.Count != 0)
                {
                    if (!cache.TryGetValue("CMAKE_HOME_DIRECTORY", out var source) || !WorkspacePathPolicy.IsSamePath(source, engine) ||
                        (string.IsNullOrWhiteSpace(cache.GetValueOrDefault("CMAKE_CONFIGURATION_TYPES")) &&
                            cache.GetValueOrDefault("CMAKE_BUILD_TYPE") != request.Configuration))
                    {
                        throw new InvalidOperationException("Use a build directory configured for this engine and configuration.");
                    }
                }
                if (request.BuildRuntime)
                {
                    progress?.Report("Building engine " + request.Configuration);
                    if (cache.Count == 0)
                    {
                        var arguments = new List<string> { "-S", engine, "-B", build,
                            "-DCMAKE_BUILD_TYPE=" + request.Configuration, "-DSAILOR_BUILD_EXECUTABLE=ON",
                            "-DSAILOR_BUILD_TESTS=OFF", "-DSAILOR_BINARIES_DIR=" + Path.Combine(build, "Binaries") };
                        AddToolchain(engine, arguments);
                        await Run(request.CMakeExecutable, arguments, engine);
                    }
                    await Run(request.CMakeExecutable, ["--build", build, "--config", request.Configuration,
                        "--target", "SailorExec", "--parallel", "4"], engine);
                }
                else if (cache.Count == 0)
                {
                    throw new InvalidOperationException("Build the engine before using --no-build-runtime.");
                }
                progress?.Report("Installing the matching engine SDK");
                await Run(request.CMakeExecutable, ["--install", build, "--config", request.Configuration, "--prefix", sdk], engine);
            }
            var executable = Path.Combine(sdk, "bin", executableName);
            if (!File.Exists(executable))
            {
                throw new FileNotFoundException("The engine SDK must include the requested standalone runtime.", executable);
            }
            var descriptionText = await Run(executable, ["--describe-workspace", workspace], workspace);
            var description = JsonSerializer.Deserialize<WorkspaceDescription>(descriptionText, JsonOptions)
                ?? throw new InvalidDataException("The engine returned no workspace description.");
            if (description.SchemaVersion != 1)
            {
                throw new InvalidDataException("Unsupported workspace description schema.");
            }
            string? module = null;
            string? moduleBuild = null;
            if (!description.Legacy)
            {
                if (!File.Exists(Path.Combine(description.GeneratedProject, "CMakeLists.txt")))
                {
                    throw new FileNotFoundException("Generate the workspace CMake project before exporting it.");
                }
                var identity = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(workspace)))[..16];
                moduleBuild = sourceEngine ? Path.Combine(build, "GameProjects", identity, request.Configuration) :
                    Path.Combine(description.Cache, "GameBuild", request.Platform.ToString(), request.Configuration);
                var project = Path.Combine(moduleBuild, "ExportProject");
                Directory.CreateDirectory(project);
                var moduleDirectory = Path.Combine(moduleBuild, "Binaries");
                var rpath = request.Platform == GameBuildPlatform.MacOS ? "@loader_path/../../lib" : "$ORIGIN/../../lib";
                var cmake = "cmake_minimum_required(VERSION 3.21)\nproject(SailorGameExport LANGUAGES CXX)\n" +
                    $"add_subdirectory(\"{CMakePath(description.GeneratedProject)}\" Project)\n" +
                    $"set_target_properties({description.LogicModuleName} PROPERTIES\n" +
                    $"  RUNTIME_OUTPUT_DIRECTORY \"{CMakePath(moduleDirectory)}/$<CONFIG>\"\n" +
                    $"  LIBRARY_OUTPUT_DIRECTORY \"{CMakePath(moduleDirectory)}/$<CONFIG>\"\n" +
                    $"  PDB_OUTPUT_DIRECTORY \"{CMakePath(moduleDirectory)}/$<CONFIG>\"\n" +
                    $"  COMPILE_PDB_OUTPUT_DIRECTORY \"{CMakePath(moduleDirectory)}/$<CONFIG>\"\n" +
                    $"  BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH \"{rpath}\")\n";
                if (request.Platform == GameBuildPlatform.Windows)
                {
                    cmake += $"file(GENERATE OUTPUT \"{CMakePath(moduleBuild)}/runtime-dlls-$<CONFIG>.txt\" " +
                        $"CONTENT \"$<JOIN:$<TARGET_RUNTIME_DLLS:{description.LogicModuleName}>,\\n>\")\n";
                }
                await File.WriteAllTextAsync(Path.Combine(project, "CMakeLists.txt"), cmake, cancellationToken);
                var arguments = new List<string> { "-S", project, "-B", Path.Combine(moduleBuild, "Build"),
                    "-DCMAKE_BUILD_TYPE=" + request.Configuration, "-DSAILOR_ENGINE_ROOT=" + sdk,
                    "-DSAILOR_ENGINE_REFERENCE_KIND=installed", "-DSailor_DIR=" + Path.Combine(sdk, "lib", "cmake", "Sailor") };
                AddToolchain(engine, arguments);
                if (sourceEngine)
                {
                    var engineCache = ReadCMakeCache(build);
                    foreach (var key in new[] { "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM", "CMAKE_CXX_FLAGS",
                        "CMAKE_CXX_FLAGS_" + request.Configuration.ToUpperInvariant(), "CMAKE_MSVC_RUNTIME_LIBRARY",
                        "CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_DEPLOYMENT_TARGET", "VCPKG_TARGET_TRIPLET", "VCPKG_HOST_TRIPLET",
                        "VCPKG_INSTALLED_DIR", "VCPKG_OVERLAY_PORTS", "VCPKG_OVERLAY_TRIPLETS" })
                    {
                        if (engineCache.TryGetValue(key, out var value))
                        {
                            arguments.Add("-D" + key + "=" + value);
                        }
                    }
                    foreach (var (key, flag) in new[] { ("CMAKE_GENERATOR", "-G"),
                        ("CMAKE_GENERATOR_PLATFORM", "-A"), ("CMAKE_GENERATOR_TOOLSET", "-T") })
                    {
                        if (engineCache.TryGetValue(key, out var value) && !string.IsNullOrWhiteSpace(value))
                        {
                            arguments.Add(flag);
                            arguments.Add(value);
                        }
                    }
                }
                progress?.Report("Building game module " + request.Configuration);
                await Run(request.CMakeExecutable, arguments, workspace);
                await Run(request.CMakeExecutable, ["--build", Path.Combine(moduleBuild, "Build"),
                    "--config", request.Configuration, "--target", description.LogicModuleName, "--parallel", "4"], workspace);
                var moduleName = request.Platform switch
                {
                    GameBuildPlatform.Windows => description.LogicModuleName + ".dll",
                    GameBuildPlatform.MacOS => "lib" + description.LogicModuleName + ".dylib",
                    _ => "lib" + description.LogicModuleName + ".so"
                };
                module = Path.Combine(moduleDirectory, request.Configuration, moduleName);
                if (!File.Exists(module))
                {
                    throw new FileNotFoundException("The game build did not produce the expected module.", module);
                }
            }
            var parent = Path.GetDirectoryName(output) ?? throw new IOException("The output has no parent directory.");
            Directory.CreateDirectory(parent);
            staging = Path.Combine(parent, ".sailor-build-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(staging);
            progress?.Report("Preparing standalone runtime");
            if (sourceEngine)
            {
                await Run(request.CMakeExecutable, ["--install", build, "--config", request.Configuration,
                    "--prefix", staging, "--component", "Runtime"], engine);
            }
            else
            {
                CopyRuntimeDirectory(Path.Combine(sdk, "bin"), Path.Combine(staging, "bin"), request.Configuration);
                CopyRuntimeDirectory(Path.Combine(sdk, "lib"), Path.Combine(staging, "lib"), request.Configuration);
            }
            if (module is not null)
            {
                var moduleDirectory = Path.Combine(staging, "Binaries", request.Configuration);
                Directory.CreateDirectory(moduleDirectory);
                File.Copy(module, Path.Combine(moduleDirectory, Path.GetFileName(module)));
                var symbols = Path.ChangeExtension(module, ".pdb");
                if (request.Platform == GameBuildPlatform.Windows && File.Exists(symbols))
                {
                    File.Copy(symbols, Path.Combine(moduleDirectory, Path.GetFileName(symbols)));
                }
                if (request.Platform == GameBuildPlatform.Windows)
                {
                    foreach (var dll in await File.ReadAllLinesAsync(Path.Combine(moduleBuild!, "runtime-dlls-" + request.Configuration + ".txt"), cancellationToken))
                    {
                        if (!string.IsNullOrWhiteSpace(dll))
                        {
                            File.Copy(dll, Path.Combine(staging, "bin", Path.GetFileName(dll)), overwrite: true);
                        }
                    }
                }
                var contentPath = description.EngineMode ? "Engine/Content" : "Content";
                var manifest = new { manifestVersion = 1, workspaceId = description.WorkspaceId, name = description.Name,
                    enginePath = "Engine", engineReferenceKind = "installed", contentPath,
                    sourcePath = "Source", generatedProjectPath = "Generated", cachePath = "Cache", buildPath = "Cache/Build",
                    logicOutputPath = "Binaries", logicModuleName = description.LogicModuleName };
                await SaveJson(Path.Combine(staging, "workspace.sailor"), manifest);
            }
            progress?.Report("Preparing a private copy of game assets");
            var archives = new List<object>();
            var roots = new List<(string Source, string Root, string Archive)>();
            if (description.Legacy || !description.EngineMode)
            {
                roots.Add((description.Content, "Content", "Content.zip"));
            }
            if (!description.Legacy)
            {
                roots.Add((description.EngineContent, "Engine/Content", "EngineContent.zip"));
            }
            var compression = request.Options.Compression.ToString().ToLowerInvariant();
            foreach (var root in roots)
            {
                var target = Path.Combine(staging, root.Root);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                await Run(executable, ["--copy-assets", root.Source, target], workspace);
            }
            if (!description.Legacy)
            {
                await Run(executable, ["--prepare-game-assets", "--workspace", Path.Combine(staging, "Engine")], staging);
            }
            if (description.Legacy || !description.EngineMode)
            {
                await Run(executable, ["--prepare-game-assets", "--workspace", staging], staging);
            }
            foreach (var cache in new[] { Path.Combine(staging, "Cache"), Path.Combine(staging, "Engine", "Cache") })
            {
                if (Directory.Exists(cache))
                {
                    Directory.Delete(cache, recursive: true);
                }
            }
            progress?.Report("Packaging prepared game assets");
            foreach (var root in roots)
            {
                var target = Path.Combine(staging, root.Root);
                if (request.Options.Assets == GameAssetPackaging.Zip)
                {
                    await Run(executable, ["--pack-assets", target, Path.Combine(staging, root.Archive), compression], workspace);
                    Directory.Delete(target, recursive: true);
                    Directory.CreateDirectory(target);
                    archives.Add(new { root = root.Root, file = root.Archive });
                }
            }
            string? initialCache = null;
            if (request.Options.IncludeInitialCache)
            {
                progress?.Report("Exporting initial shader and model cache");
                initialCache = "InitialCache.zip";
                Directory.CreateDirectory(Path.Combine(staging, "InitialCache"));
                await Run(executable, ["--pack-cache", workspace, Path.Combine(staging, initialCache), compression], workspace);
            }
            await SaveJson(Path.Combine(staging, "GamePackage.json"), new { packageVersion = 1,
                packageId = Guid.NewGuid().ToString("N"), archives, initialCache });
            var settings = Path.Combine(workspace, "ProjectSettings.yaml");
            if (File.Exists(settings))
            {
                File.Copy(settings, Path.Combine(staging, "ProjectSettings.yaml"));
            }
            var licenses = Path.Combine(engine, "ThirdPartyLicenses.txt");
            if (!File.Exists(licenses))
            {
                throw new FileNotFoundException("The engine distribution must include ThirdPartyLicenses.txt.", licenses);
            }
            File.Copy(licenses, Path.Combine(staging, "ThirdPartyLicenses.txt"), overwrite: true);
            if (request.Platform == GameBuildPlatform.Linux)
            {
                await CollectLinuxDependencies(staging, sdk, Run);
            }
            else if (request.Platform == GameBuildPlatform.MacOS)
            {
                await FixMacLibraries(staging, Run);
            }
            progress?.Report("Validating the packaged game module");
            await Run(Path.Combine(staging, "bin", executableName), ["--validate-workspace-module", staging], staging);
            await WriteLauncher(staging, executableName, request.Platform);
            cancellationToken.ThrowIfCancellationRequested();
            Directory.Move(staging, output);
            staging = null;
            progress?.Report("Game build: " + output);
            return new GameBuildResult(true, output);

            async Task<string> Run(string program, IReadOnlyList<string> arguments, string workingDirectory)
            {
                cancellationToken.ThrowIfCancellationRequested();
                progress?.Report(program + " " + string.Join(" ", arguments.Select(argument => JsonSerializer.Serialize(argument))));
                var invocation = new WorkspaceProcessInvocation(program, arguments, workingDirectory)
                {
                    OutputEncoding = Path.GetFileName(program).StartsWith("SailorEngine-", StringComparison.Ordinal) ? Encoding.UTF8 : null
                };
                var result = await runner.RunAsync(invocation, cancellationToken);
                if (!string.IsNullOrWhiteSpace(result.Output))
                {
                    progress?.Report(result.Output);
                }
                if (!result.Succeeded)
                {
                    throw new InvalidOperationException($"{program} exited {result.ExitCode}.\n{result.Output}");
                }
                return result.Output;
            }

            Task SaveJson<T>(string path, T value) => File.WriteAllTextAsync(path,
                JsonSerializer.Serialize(value, JsonOptions) + "\n", cancellationToken);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or
            InvalidOperationException or JsonException)
        {
            return new GameBuildResult(false, request.OutputDirectory, error.Message);
        }
        finally
        {
            try
            {
                if (staging is not null)
                {
                    Directory.Delete(staging, recursive: true);
                }
            }
            finally
            {
                _gate.Release();
            }
        }
    }

    static Dictionary<string, string> ReadCMakeCache(string build)
    {
        var file = Path.Combine(build, "CMakeCache.txt");
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        if (File.Exists(file))
        {
            foreach (var line in File.ReadLines(file))
            {
                var colon = line.IndexOf(':');
                var equals = line.IndexOf('=');
                if (!line.StartsWith("//", StringComparison.Ordinal) && colon > 0 && equals > colon)
                {
                    result[line[..colon]] = line[(equals + 1)..];
                }
            }
        }
        return result;
    }

    static void AddToolchain(string engine, List<string> arguments)
    {
        var path = Path.Combine(engine, "External", "vcpkg", "scripts", "buildsystems", "vcpkg.cmake");
        if (File.Exists(path))
        {
            arguments.Add("-DCMAKE_TOOLCHAIN_FILE=" + path);
        }
    }

    static string CMakePath(string path) => path.Replace('\\', '/').Replace("$", "\\$").Replace(";", "\\;").Replace("\"", "\\\"");

    static void CopyRuntimeDirectory(string source, string destination, string configuration)
    {
        Directory.CreateDirectory(destination);
        if (!Directory.Exists(source))
        {
            return;
        }
        foreach (var file in Directory.EnumerateFiles(source))
        {
            var name = Path.GetFileName(file);
            if ((name.StartsWith("Sailor-", StringComparison.Ordinal) || name.StartsWith("SailorEngine-", StringComparison.Ordinal)) &&
                !name.Contains("-" + configuration + ".", StringComparison.Ordinal) && !name.EndsWith("-" + configuration, StringComparison.Ordinal))
            {
                continue;
            }
            File.Copy(file, Path.Combine(destination, name));
        }
    }

    static async Task CollectLinuxDependencies(string staging, string sdk,
        Func<string, IReadOnlyList<string>, string, Task<string>> run)
    {
        var libraries = Path.Combine(staging, "lib");
        Directory.CreateDirectory(libraries);
        var images = Directory.EnumerateFiles(staging, "*.so*", SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(Path.Combine(staging, "bin"), "SailorEngine-*"));
        foreach (var image in images.ToArray())
        {
            var text = await run("/usr/bin/env", ["LD_LIBRARY_PATH=" + Path.Combine(sdk, "lib"), "/usr/bin/ldd", image], staging);
            foreach (var line in text.Split('\n'))
            {
                if (line.Contains("=> not found", StringComparison.Ordinal))
                {
                    throw new IOException("Unresolved runtime dependency: " + line.Trim());
                }
                var tail = line.Contains("=>", StringComparison.Ordinal) ? line[(line.IndexOf("=>", StringComparison.Ordinal) + 2)..].Trim() : line.Trim();
                var path = tail.Split(" (", StringSplitOptions.None)[0];
                if (!Path.IsPathRooted(path) || path.StartsWith("/usr/lib/", StringComparison.Ordinal) ||
                    path.StartsWith("/lib/", StringComparison.Ordinal) || path.StartsWith("/lib64/", StringComparison.Ordinal))
                {
                    continue;
                }
                var destination = Path.Combine(libraries, Path.GetFileName(path));
                if (!File.Exists(destination))
                {
                    File.Copy(path, destination);
                }
            }
        }
    }

    static async Task FixMacLibraries(string staging, Func<string, IReadOnlyList<string>, string, Task<string>> run)
    {
        var libraries = Path.Combine(staging, "lib");
        Directory.CreateDirectory(libraries);
        var pending = new Queue<string>(Directory.EnumerateFiles(staging, "*.dylib", SearchOption.AllDirectories)
            .Concat(Directory.EnumerateFiles(Path.Combine(staging, "bin"), "SailorEngine-*")));
        var images = new HashSet<string>(StringComparer.Ordinal);
        while (pending.TryDequeue(out var image))
        {
            if (!images.Add(image))
            {
                continue;
            }
            if (OperatingSystem.IsMacOS() || OperatingSystem.IsMacCatalyst())
            {
                File.SetUnixFileMode(image, File.GetUnixFileMode(image) | UnixFileMode.UserWrite);
            }
            var text = await run("/usr/bin/otool", ["-L", image], staging);
            // A dylib's first otool entry is its own install ID.
            foreach (var line in text.Split('\n').Skip(image.EndsWith(".dylib", StringComparison.Ordinal) ? 2 : 1))
            {
                var dependency = line.Trim().Split(" (", StringSplitOptions.None)[0];
                if (!dependency.StartsWith("/", StringComparison.Ordinal) || dependency.StartsWith("/System/Library/", StringComparison.Ordinal) ||
                    dependency.StartsWith("/usr/lib/", StringComparison.Ordinal))
                {
                    continue;
                }
                var name = Path.GetFileName(dependency);
                var target = Path.Combine(libraries, name);
                if (!File.Exists(target))
                {
                    File.Copy(dependency, target);
                    pending.Enqueue(target);
                }
                await run("/usr/bin/install_name_tool", ["-change", dependency, "@rpath/" + name, image], staging);
            }
        }
        foreach (var image in images)
        {
            if (image.EndsWith(".dylib", StringComparison.Ordinal))
            {
                await run("/usr/bin/install_name_tool", ["-id", "@rpath/" + Path.GetFileName(image), image], staging);
            }
            await run("/usr/bin/codesign", ["--force", "--sign", "-", image], staging);
        }
    }

    static async Task WriteLauncher(string root, string executable, GameBuildPlatform platform)
    {
        if (platform == GameBuildPlatform.Windows)
        {
            await File.WriteAllTextAsync(Path.Combine(root, "Sailor.cmd"),
                "@echo off\r\n\"%~dp0bin\\" + executable + "\" --workspace \"%~dp0.\" %*\r\n");
        }
        else
        {
            var name = platform == GameBuildPlatform.MacOS ? "Sailor.command" : "Sailor.sh";
            var file = Path.Combine(root, name);
            await File.WriteAllTextAsync(file, "#!/bin/sh\nset -eu\npackage_dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n" +
                (platform == GameBuildPlatform.Linux ? "export LD_LIBRARY_PATH=\"$package_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n" : "") +
                "exec \"$package_dir/bin/" + executable + "\" --workspace \"$package_dir\" \"$@\"\n");
            if (!OperatingSystem.IsWindows())
            {
                File.SetUnixFileMode(file, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                    UnixFileMode.GroupRead | UnixFileMode.GroupExecute | UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
            }
        }
    }
}
