#nullable enable

using System.Diagnostics;
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
    public string CMakeExecutable { get; init; } = GameBuildEnvironment.CMakeExecutable;
    public string StartupScene { get; init; } = string.Empty;
    public bool BuildRuntime { get; init; } = true;
}

public sealed record GameBuildResult(bool Succeeded, string OutputDirectory, string? Error = null)
{
    public string? LogFile { get; init; }
    public WorkspaceProcessInvocation? RunInvocation { get; init; }
}

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
        string? logFile = null;
        try
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(request.WorkspaceDirectory);
            ArgumentException.ThrowIfNullOrWhiteSpace(request.EngineDirectory);
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
            var logs = Path.Combine(workspace, "Cache", "BuildLogs");
            Directory.CreateDirectory(logs);
            logFile = Path.Combine(logs, DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..8] + ".log");
            Report($"{request.Platform} / {request.Configuration} — {request.StartupScene}");
            var toolchainFile = string.IsNullOrWhiteSpace(request.Options.ToolchainFile) ? string.Empty :
                Path.GetFullPath(request.Options.ToolchainFile, workspace);
            var capability = await GameBuildEnvironment.CheckAsync(workspace, request.Platform,
                toolchainFile, runner, request.CMakeExecutable, cancellationToken);
            if (!capability.CanBuild)
            {
                throw new InvalidOperationException(capability.Detail);
            }
            var sourceEngine = File.Exists(Path.Combine(engine, "CMakeLists.txt")) && Directory.Exists(Path.Combine(engine, "Runtime"));
            var configuredBuild = request.EngineBuildDirectory ??
                (string.IsNullOrWhiteSpace(request.Options.EngineBuildDirectory) ? null : request.Options.EngineBuildDirectory);
            var build = Path.GetFullPath(configuredBuild ?? GameBuildEnvironment.FindEngineBuild(
                engine, request.Platform, request.Configuration, toolchainFile), engine);
            var sdk = sourceEngine ? Path.Combine(build, "GameSDK", request.Configuration) : engine;
            var suffix = request.Platform == GameBuildPlatform.Windows ? ".exe" : "";
            var executableName = "SailorEngine-" + request.Configuration + suffix;
            if (sourceEngine)
            {
                var cache = GameBuildEnvironment.ReadCMakeCache(build);
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
                    Report("Building engine " + request.Configuration);
                    if (cache.Count == 0)
                    {
                        var arguments = new List<string> { "-S", engine, "-B", build,
                            "-DCMAKE_BUILD_TYPE=" + request.Configuration, "-DSAILOR_BUILD_EXECUTABLE=ON",
                            "-DSAILOR_BUILD_TESTS=OFF", "-DSAILOR_BINARIES_DIR=" + Path.Combine(build, "Binaries") };
                        AddToolchain(engine, arguments, toolchainFile);
                        await Run(request.CMakeExecutable, arguments, engine);
                    }
                    await Run(request.CMakeExecutable, ["--build", build, "--config", request.Configuration,
                        "--target", "SailorExec", "--parallel", "4"], engine);
                }
                else if (cache.Count == 0)
                {
                    throw new InvalidOperationException("Build the engine before using --no-build-runtime.");
                }
                Report("Installing the matching engine SDK");
                await Run(request.CMakeExecutable, ["--install", build, "--config", request.Configuration, "--prefix", sdk], engine);
            }
            var executable = Path.Combine(sdk, "bin", executableName);
            if (!File.Exists(executable))
            {
                throw new FileNotFoundException("The engine SDK must include the requested standalone runtime.", executable);
            }
            var descriptionText = await RunTarget(executable, ["--describe-workspace", workspace], workspace);
            var description = JsonSerializer.Deserialize<WorkspaceDescription>(descriptionText, JsonOptions)
                ?? throw new InvalidDataException("The engine returned no workspace description.");
            if (description.SchemaVersion != 1)
            {
                throw new InvalidDataException("Unsupported workspace description schema.");
            }
            var startupScene = ValidateStartupScene(description.Content, request.StartupScene);
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
                AddToolchain(engine, arguments, toolchainFile);
                if (sourceEngine)
                {
                    var engineCache = GameBuildEnvironment.ReadCMakeCache(build);
                    foreach (var key in new[] { "CMAKE_CXX_COMPILER", "CMAKE_MAKE_PROGRAM", "CMAKE_CXX_FLAGS",
                        "CMAKE_CXX_FLAGS_" + request.Configuration.ToUpperInvariant(), "CMAKE_MSVC_RUNTIME_LIBRARY",
                        "CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_DEPLOYMENT_TARGET", "CMAKE_SYSROOT",
                        "CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_PROCESSOR", "CMAKE_CROSSCOMPILING_EMULATOR", "VCPKG_CHAINLOAD_TOOLCHAIN_FILE", "VCPKG_TARGET_TRIPLET", "VCPKG_HOST_TRIPLET",
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
                Report("Building game module " + request.Configuration);
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
            Report("Preparing standalone runtime");
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
            Report("Preparing a private copy of game assets");
            var archives = new List<object>();
            var roots = new List<(string Source, string Root, string Archive)>();
            if (description.Legacy || !description.EngineMode)
            {
                roots.Add((description.Content, "Content", "Content.bundle"));
            }
            if (!description.Legacy)
            {
                roots.Add((description.EngineContent, "Engine/Content", "EngineContent.bundle"));
            }
            var compression = request.Options.Compression.ToString().ToLowerInvariant();
            foreach (var root in roots)
            {
                var target = Path.Combine(staging, root.Root);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                await RunTarget(executable, ["--copy-assets", root.Source, target], workspace);
            }
            if (!description.Legacy)
            {
                await RunTarget(executable, ["--prepare-game-assets", "--workspace", Path.Combine(staging, "Engine")], staging);
            }
            if (description.Legacy || !description.EngineMode)
            {
                await RunTarget(executable, ["--prepare-game-assets", "--workspace", staging], staging);
            }
            foreach (var cache in new[] { Path.Combine(staging, "Cache"), Path.Combine(staging, "Engine", "Cache") })
            {
                if (Directory.Exists(cache))
                {
                    Directory.Delete(cache, recursive: true);
                }
            }
            Report("Packaging prepared game assets");
            foreach (var root in roots)
            {
                var target = Path.Combine(staging, root.Root);
                if (request.Options.Assets == GameAssetPackaging.Zip)
                {
                    await RunTarget(executable, ["--pack-assets", target, Path.Combine(staging, root.Archive), compression], workspace);
                    Directory.Delete(target, recursive: true);
                    archives.Add(new { root = root.Root, file = root.Archive });
                }
            }
            var emptyEngine = Path.Combine(staging, "Engine");
            if (Directory.Exists(emptyEngine) && !Directory.EnumerateFileSystemEntries(emptyEngine).Any())
            {
                Directory.Delete(emptyEngine);
            }
            string? initialCache = null;
            if (request.Options.IncludeInitialCache)
            {
                Report("Exporting initial shader and model cache");
                initialCache = "InitialCache.bundle";
                await RunTarget(executable, ["--pack-cache", workspace, Path.Combine(staging, initialCache), compression], workspace);
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
                await CollectLinuxDependencies(staging, sdk, RunTarget);
            }
            else if (request.Platform == GameBuildPlatform.MacOS)
            {
                await FixMacLibraries(staging, RunTarget);
            }
            Report("Validating the packaged game module");
            await RunTarget(Path.Combine(staging, "bin", executableName), ["--validate-workspace-module", staging], staging);
            await WriteLauncher(staging, executableName, request.Platform, startupScene);
            cancellationToken.ThrowIfCancellationRequested();
            Directory.Move(staging, output);
            staging = null;
            Report("Game build: " + output);
            return new GameBuildResult(true, output)
            {
                LogFile = logFile,
                RunInvocation = CreateRunInvocation(output, executableName, startupScene, capability.TargetRunner)
            };

            Task<string> RunTarget(string program, IReadOnlyList<string> arguments, string workingDirectory)
            {
                return capability.TargetRunner.Count == 0 ? Run(program, arguments, workingDirectory, target: true) :
                    Run(capability.TargetRunner[0], capability.TargetRunner.Skip(1).Concat(new[] { program }).Concat(arguments).ToArray(), workingDirectory, target: true);
            }

            async Task<string> Run(string program, IReadOnlyList<string> arguments, string workingDirectory, bool target = false)
            {
                cancellationToken.ThrowIfCancellationRequested();
                Report(program + " " + string.Join(" ", arguments.Select(argument => JsonSerializer.Serialize(argument))));
                var invocation = new WorkspaceProcessInvocation(program, arguments, workingDirectory)
                {
                    OutputEncoding = target || Path.GetFileName(program).StartsWith("SailorEngine-", StringComparison.Ordinal) ? Encoding.UTF8 : null
                };
                var result = await runner.RunAsync(invocation, cancellationToken);
                if (!string.IsNullOrWhiteSpace(result.Output))
                {
                    Report(result.Output);
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
            InvalidOperationException or InvalidDataException or JsonException)
        {
            Report("Build failed: " + error.Message);
            return new GameBuildResult(false, request.OutputDirectory, error.Message) { LogFile = logFile };
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

        void Report(string message)
        {
            if (logFile is not null)
            {
                File.AppendAllText(logFile, message + Environment.NewLine);
            }
            progress?.Report(message);
        }
    }

    public static int Run(GameBuildResult result)
    {
        if (!result.Succeeded || result.RunInvocation is not { } invocation)
        {
            throw new InvalidOperationException("Build the game successfully before running it.");
        }
        var start = new ProcessStartInfo(invocation.FileName)
        {
            WorkingDirectory = invocation.WorkingDirectory,
            UseShellExecute = false
        };
        foreach (var argument in invocation.Arguments)
        {
            start.ArgumentList.Add(argument);
        }
        using var process = Process.Start(start) ?? throw new InvalidOperationException("The game did not start.");
        return process.Id;
    }

    static WorkspaceProcessInvocation CreateRunInvocation(string output, string executable, string scene,
        IReadOnlyList<string> targetRunner)
    {
        var arguments = new List<string> { "--workspace", output };
        if (scene.Length != 0)
        {
            arguments.AddRange(new[] { "--world", scene });
        }
        else
        {
            arguments.Add("--new-world");
        }
        var program = Path.Combine(output, "bin", executable);
        return targetRunner.Count == 0 ? new(program, arguments, output) :
            new(targetRunner[0], targetRunner.Skip(1).Concat(new[] { program }).Concat(arguments).ToArray(), output);
    }

    static string ValidateStartupScene(string content, string scene)
    {
        if (string.IsNullOrEmpty(scene))
        {
            return string.Empty;
        }
        var file = Path.GetFullPath(scene, content);
        if (!scene.EndsWith(".world", StringComparison.OrdinalIgnoreCase) || Path.IsPathRooted(scene) ||
            scene.Any(char.IsControl) || scene.Contains('"') ||
            !WorkspacePathPolicy.IsInsideRoot(content, file) || !File.Exists(file))
        {
            throw new InvalidDataException("Choose an existing startup scene inside the project Content directory.");
        }
        return Path.GetRelativePath(content, file).Replace('\\', '/');
    }

    static void AddToolchain(string engine, List<string> arguments, string toolchainFile)
    {
        var path = string.IsNullOrWhiteSpace(toolchainFile) ?
            Path.Combine(engine, "External", "vcpkg", "scripts", "buildsystems", "vcpkg.cmake") :
            Path.GetFullPath(toolchainFile);
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

    static async Task WriteLauncher(string root, string executable, GameBuildPlatform platform, string startupScene)
    {
        if (platform == GameBuildPlatform.Windows)
        {
            await File.WriteAllTextAsync(Path.Combine(root, "Sailor.cmd"),
                "@echo off\r\n\"%~dp0bin\\" + executable + "\" --workspace \"%~dp0.\"" +
                (startupScene.Length == 0 ? " --new-world" : " --world \"" + startupScene.Replace("%", "%%") + "\"") + " %*\r\n");
        }
        else
        {
            var name = platform == GameBuildPlatform.MacOS ? "Sailor.command" : "Sailor.sh";
            var file = Path.Combine(root, name);
            await File.WriteAllTextAsync(file, "#!/bin/sh\nset -eu\npackage_dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n" +
                (platform == GameBuildPlatform.Linux ? "export LD_LIBRARY_PATH=\"$package_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"\n" : "") +
                "exec \"$package_dir/bin/" + executable + "\" --workspace \"$package_dir\"" +
                (startupScene.Length == 0 ? " --new-world" : " --world '" + startupScene.Replace("'", "'\"'\"'") + "'") + " \"$@\"\n");
            if (!OperatingSystem.IsWindows())
            {
                File.SetUnixFileMode(file, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute |
                    UnixFileMode.GroupRead | UnixFileMode.GroupExecute | UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
            }
        }
    }
}
