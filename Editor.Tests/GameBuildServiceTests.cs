using System.IO.Compression;
using System.Text.Json;
using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class GameBuildServiceTests : IDisposable
{
    readonly string root = Path.Combine(Path.GetTempPath(), "sailor-game-build-" + Guid.NewGuid().ToString("N"));
    readonly FakeRunner runner;
    string Workspace => Path.Combine(root, "project Я");
    string Engine => Path.Combine(root, "sdk Я");
    string Output => Path.Combine(root, "export Я");

    public GameBuildServiceTests()
    {
        Directory.CreateDirectory(Path.Combine(Workspace, "Content"));
        Directory.CreateDirectory(Path.Combine(Engine, "Content"));
        Directory.CreateDirectory(Path.Combine(Engine, "bin"));
        Directory.CreateDirectory(Path.Combine(Workspace, "Generated"));
        Directory.CreateDirectory(Path.Combine(Workspace, "Cache"));
        File.WriteAllText(Path.Combine(Workspace, "Generated", "CMakeLists.txt"), "project(Game)");
        File.WriteAllText(Path.Combine(Workspace, "Content", "game.txt"), "workspace asset Я");
        File.WriteAllText(Path.Combine(Engine, "Content", "engine.txt"), "engine asset");
        File.WriteAllText(Path.Combine(Engine, "ThirdPartyLicenses.txt"), "licenses");
        var executable = "SailorEngine-Release" + (OperatingSystem.IsWindows() ? ".exe" : "");
        File.WriteAllText(Path.Combine(Engine, "bin", executable), "runtime");
        runner = new FakeRunner(Workspace, Engine);
    }

    GameBuildRequest Request(GamePlatformBuildSettings? settings = null) => new(Workspace, Engine, Output,
        "Release", GameBuildSettings.HostPlatform, settings ?? new());

    [Fact]
    public async Task LooseBuildContainsIndependentManifestModuleRuntimeAndBothAssetRoots()
    {
        var result = await new GameBuildService(runner).BuildAsync(Request());
        Assert.True(result.Succeeded, result.Error);
        Assert.Equal("workspace asset Я", File.ReadAllText(Path.Combine(Output, "Content", "game.txt")));
        Assert.Equal("engine asset", File.ReadAllText(Path.Combine(Output, "Engine", "Content", "engine.txt")));
        Assert.Equal("licenses", File.ReadAllText(Path.Combine(Output, "ThirdPartyLicenses.txt")));
        Assert.True(File.Exists(Path.Combine(Output, "Binaries", "Release", FakeRunner.ModuleName)));
        using var manifest = JsonDocument.Parse(File.ReadAllText(Path.Combine(Output, "workspace.sailor")));
        Assert.Equal(1, manifest.RootElement.GetProperty("manifestVersion").GetInt32());
        Assert.Equal("Engine", manifest.RootElement.GetProperty("enginePath").GetString());
        Assert.Equal("installed", manifest.RootElement.GetProperty("engineReferenceKind").GetString());
        Assert.Equal("workspace-id", manifest.RootElement.GetProperty("workspaceId").GetString());
        using var package = JsonDocument.Parse(File.ReadAllText(Path.Combine(Output, "GamePackage.json")));
        Assert.Equal(1, package.RootElement.GetProperty("packageVersion").GetInt32());
        Assert.Empty(package.RootElement.GetProperty("archives").EnumerateArray());
        Assert.Equal(JsonValueKind.Null, package.RootElement.GetProperty("initialCache").ValueKind);
        Assert.Matches("^[a-f0-9]{32}$", package.RootElement.GetProperty("packageId").GetString()!);
        Assert.False(Directory.Exists(Path.Combine(Output, "Cache")));
        Assert.DoesNotContain(runner.Invocations, invocation => invocation.Arguments.Contains("--pack-assets"));
        Assert.Contains(runner.Invocations, invocation => invocation.Arguments.Contains("--target") && invocation.Arguments.Contains("Game"));
        Assert.Contains(runner.Invocations, invocation => invocation.Arguments.Contains("--validate-workspace-module") &&
            Path.GetDirectoryName(Path.GetDirectoryName(invocation.FileName)) == invocation.WorkingDirectory);
        var prepare = runner.Invocations.Where(invocation => invocation.Arguments.Contains("--prepare-game-assets")).ToArray();
        Assert.Equal(2, prepare.Length);
        Assert.All(prepare, invocation => Assert.True(WorkspacePathPolicy.IsInsideRoot(invocation.WorkingDirectory, invocation.Arguments[^1])));
        Assert.DoesNotContain(prepare, invocation => invocation.Arguments.Contains(Workspace) || invocation.Arguments.Contains(Engine));
    }

    [Theory]
    [InlineData(GameArchiveCompression.Stored)]
    [InlineData(GameArchiveCompression.Fastest)]
    [InlineData(GameArchiveCompression.Balanced)]
    [InlineData(GameArchiveCompression.Smallest)]
    public async Task ZipAndInitialCacheOptionsReachTheEngineAndEmittedPackage(GameArchiveCompression compression)
    {
        var result = await new GameBuildService(runner).BuildAsync(Request(new()
        {
            Assets = GameAssetPackaging.Zip, Compression = compression, IncludeInitialCache = true
        }));
        Assert.True(result.Succeeded, result.Error);
        using var package = JsonDocument.Parse(File.ReadAllText(Path.Combine(Output, "GamePackage.json")));
        Assert.Equal(2, package.RootElement.GetProperty("archives").GetArrayLength());
        Assert.Equal("InitialCache.bundle", package.RootElement.GetProperty("initialCache").GetString());
        Assert.False(Directory.Exists(Path.Combine(Output, "Content")));
        Assert.False(Directory.Exists(Path.Combine(Output, "Engine")));
        Assert.False(Directory.Exists(Path.Combine(Output, "InitialCache")));
        Assert.Empty(Directory.EnumerateFiles(Output, "*.zip", SearchOption.AllDirectories));
        Assert.Equal(new[] { "Content.bundle", "EngineContent.bundle", "InitialCache.bundle" },
            Directory.EnumerateFiles(Output, "*.bundle").Select(Path.GetFileName).Order(StringComparer.Ordinal));
        foreach (var invocation in runner.Invocations.Where(invocation => invocation.Arguments[0] is "--pack-assets" or "--pack-cache"))
        {
            Assert.Equal(compression.ToString().ToLowerInvariant(), invocation.Arguments[3]);
        }
        Assert.Equal(3, runner.Invocations.Count(invocation => invocation.Arguments[0] is "--pack-assets" or "--pack-cache"));
        using var zip = ZipFile.OpenRead(Path.Combine(Output, "Content.bundle"));
        Assert.NotNull(zip.GetEntry("game.txt"));
    }

    [Fact]
    public async Task FailedArchiveCommandPreservesExistingFilesAndRemovesOnlyStaging()
    {
        var sentinel = Path.Combine(root, "unrelated.txt");
        File.WriteAllText(sentinel, "keep");
        runner.FailArchive = true;
        var result = await new GameBuildService(runner).BuildAsync(Request(new() { Assets = GameAssetPackaging.Zip }));
        Assert.False(result.Succeeded);
        Assert.Contains("archive failure", result.Error);
        Assert.False(Directory.Exists(Output));
        Assert.Empty(Directory.EnumerateDirectories(root, ".sailor-build-*"));
        Assert.Equal("keep", File.ReadAllText(sentinel));
    }

    [Theory]
    [InlineData("--prepare-game-assets")]
    [InlineData("--validate-workspace-module")]
    public async Task FailedPreparationOrModuleValidationDoesNotPublish(string command)
    {
        runner.FailCommand = command;
        var result = await new GameBuildService(runner).BuildAsync(Request());
        Assert.False(result.Succeeded);
        Assert.Contains("tool validation failed", result.Error);
        Assert.False(Directory.Exists(Output));
        Assert.Empty(Directory.EnumerateDirectories(root, ".sailor-build-*"));
        Assert.Equal("workspace asset Я", File.ReadAllText(Path.Combine(Workspace, "Content", "game.txt")));
    }

    [Fact]
    public async Task ExistingOrContainedOutputIsRejectedBeforeStartingAnyProcess()
    {
        var service = new GameBuildService(runner);
        Directory.CreateDirectory(Output);
        File.WriteAllText(Path.Combine(Output, "keep"), "keep");
        Assert.False((await service.BuildAsync(Request())).Succeeded);
        Assert.Equal("keep", File.ReadAllText(Path.Combine(Output, "keep")));
        Assert.False((await service.BuildAsync(Request() with { OutputDirectory = Path.Combine(Workspace, "Build") })).Succeeded);
        Assert.Empty(runner.Invocations);
    }

    [Fact]
    public async Task CancellationRemovesStagingAndAllowsNextBuild()
    {
        var service = new GameBuildService(runner);
        using var cancellation = new CancellationTokenSource();
        runner.BeforeArchive = () => cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => service.BuildAsync(
            Request(new() { Assets = GameAssetPackaging.Zip }), cancellationToken: cancellation.Token));
        Assert.Empty(Directory.EnumerateDirectories(root, ".sailor-build-*"));
        Assert.False(Directory.Exists(Output));
        runner.BeforeArchive = null;
        var retry = await service.BuildAsync(Request());
        Assert.True(retry.Succeeded, retry.Error);
    }

    [Fact]
    public void PlatformSettingsRoundTripAndRejectUnversionedDuplicateOrUnknownData()
    {
        var settings = new GameBuildSettings { Configuration = "Debug", StartupScene = "Levels/Start Я.world" };
        settings.Platforms[GameBuildPlatform.Windows] = new() { Assets = GameAssetPackaging.Zip, Compression = GameArchiveCompression.Stored };
        settings.Platforms[GameBuildPlatform.Linux] = new() { IncludeInitialCache = true, OutputDirectory = "/output Я", ToolchainFile = "/cross Я.cmake", EngineBuildDirectory = "/engine build Я" };
        GameBuildSettingsStore.Save(Workspace, settings);
        var loaded = GameBuildSettingsStore.Load(Workspace);
        Assert.Equal("Debug", loaded.Configuration);
        Assert.Equal(settings.StartupScene, loaded.StartupScene);
        Assert.Equal("/cross Я.cmake", loaded.Platforms[GameBuildPlatform.Linux].ToolchainFile);
        Assert.Equal("/engine build Я", loaded.Platforms[GameBuildPlatform.Linux].EngineBuildDirectory);
        Assert.Equal(GameArchiveCompression.Stored, loaded.Platforms[GameBuildPlatform.Windows].Compression);
        Assert.True(loaded.Platforms[GameBuildPlatform.Linux].IncludeInitialCache);
        Assert.Equal(GameAssetPackaging.Folders, loaded.Platforms[GameBuildPlatform.MacOS].Assets);
        foreach (var payload in new[] { "{}", "{\"schemaVersion\":2}", "{\"schemaVersion\":1,\"schemaVersion\":1}",
            "{\"schemaVersion\":1,\"unexpected\":true}" })
        {
            File.WriteAllText(Path.Combine(Workspace, GameBuildSettingsStore.FileName), payload);
            Assert.ThrowsAny<Exception>(() => GameBuildSettingsStore.Load(Workspace));
        }
    }

    [Theory]
    [InlineData("Debug")]
    [InlineData("Release")]
    public async Task StartupSceneAndConfigurationReachTheExportedGameAndLauncher(string configuration)
    {
        const string scene = "Levels/Первый ' остров.world";
        var sceneFile = Path.Combine(Workspace, "Content", scene);
        Directory.CreateDirectory(Path.GetDirectoryName(sceneFile)!);
        File.WriteAllText(sceneFile, "scene");
        var executable = "SailorEngine-" + configuration + (OperatingSystem.IsWindows() ? ".exe" : "");
        File.WriteAllText(Path.Combine(Engine, "bin", executable), "runtime");
        var result = await new GameBuildService(runner).BuildAsync(Request() with
        {
            Configuration = configuration, StartupScene = scene
        });
        Assert.True(result.Succeeded, result.Error);
        var invocation = Assert.IsType<WorkspaceProcessInvocation>(result.RunInvocation);
        Assert.Equal(Path.Combine(result.OutputDirectory, "bin", executable), invocation.FileName);
        Assert.Equal(result.OutputDirectory, invocation.WorkingDirectory);
        Assert.Equal(new[] { "--workspace", result.OutputDirectory, "--world", scene }, invocation.Arguments);
        Assert.Contains(runner.Invocations, command => command.Arguments.Contains("--config") && command.Arguments.Contains(configuration));
        Assert.True(File.Exists(result.LogFile));
        Assert.True(File.Exists(Path.Combine(Output, "Content", scene)));
        if (!OperatingSystem.IsWindows())
        {
            // Execute the emitted shell launcher, including quoting, instead of checking its source text.
            File.WriteAllText(invocation.FileName, "#!/bin/sh\nprintf '%s\\n' \"$@\" > launch-arguments.txt\n");
            File.SetUnixFileMode(invocation.FileName, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            var launcher = Path.Combine(Output, OperatingSystem.IsMacOS() ? "Sailor.command" : "Sailor.sh");
            var launch = await new WorkspaceProcessRunner().RunAsync(new("/bin/sh", [launcher], Output));
            Assert.True(launch.Succeeded, launch.Output);
            var actual = File.ReadAllLines(Path.Combine(Output, "launch-arguments.txt"));
            Assert.Equal("--workspace", actual[0]);
            Assert.True(WorkspacePathPolicy.IsSamePath(result.OutputDirectory, actual[1]));
            Assert.Equal(new[] { "--world", scene }, actual.Skip(2));
        }
    }

    [Theory]
    [InlineData("../outside.world")]
    [InlineData("missing.world")]
    [InlineData("asset.txt")]
    public async Task InvalidStartupSceneDoesNotPublishOrCompileTheModule(string scene)
    {
        var result = await new GameBuildService(runner).BuildAsync(Request() with { StartupScene = scene });
        Assert.False(result.Succeeded);
        Assert.Contains("startup scene", result.Error);
        Assert.False(Directory.Exists(Output));
        Assert.DoesNotContain(runner.Invocations, invocation => invocation.Arguments.Contains("--build"));
        Assert.Contains("Build failed", File.ReadAllText(result.LogFile!));
    }

    [Fact]
    public async Task ConfiguredCrossToolchainRunsTargetToolsThroughItsRunnerAndKeepsCMakeNative()
    {
        var platform = GameBuildSettings.HostPlatform == GameBuildPlatform.Linux ? GameBuildPlatform.Windows : GameBuildPlatform.Linux;
        var toolchain = Path.Combine(root, "target toolchain.cmake");
        var emulator = Path.Combine(root, "target runner" + (OperatingSystem.IsWindows() ? ".exe" : ""));
        File.WriteAllText(toolchain, "toolchain fixture");
        File.WriteAllText(emulator, "runner fixture");
        runner.Platform = platform;
        runner.TargetRunner = emulator;
        File.WriteAllText(Path.Combine(Engine, "bin", "SailorEngine-Release" + (platform == GameBuildPlatform.Windows ? ".exe" : "")), "runtime");
        var result = await new GameBuildService(runner).BuildAsync(Request(new() { ToolchainFile = toolchain }) with { Platform = platform });
        Assert.True(result.Succeeded, result.Error);
        var describe = Assert.Single(runner.Invocations, invocation => invocation.Arguments.Contains("--describe-workspace"));
        Assert.Equal(emulator, describe.FileName);
        Assert.Contains(runner.Invocations, invocation => invocation.FileName == emulator && invocation.Arguments.Contains("--validate-workspace-module"));
        Assert.Contains(runner.Invocations, invocation => invocation.FileName != emulator && invocation.Arguments.Contains("-DCMAKE_TOOLCHAIN_FILE=" + toolchain));
        Assert.Equal(emulator, result.RunInvocation!.FileName);
        Assert.Contains(Path.Combine(result.OutputDirectory, "bin", "SailorEngine-Release" + (platform == GameBuildPlatform.Windows ? ".exe" : "")), result.RunInvocation.Arguments);
    }

    [Fact]
    public async Task MissingCrossToolchainExplainsUnavailabilityBeforeInvokingTargetBinaries()
    {
        var platform = GameBuildSettings.HostPlatform == GameBuildPlatform.Linux ? GameBuildPlatform.Windows : GameBuildPlatform.Linux;
        var result = await new GameBuildService(runner).BuildAsync(Request() with { Platform = platform });
        Assert.False(result.Succeeded);
        Assert.Contains("toolchain", result.Error);
        Assert.Empty(runner.Invocations);
    }

    [Fact]
    public async Task CrossToolchainMustMatchSelectedPlatformAndHaveAnInstalledRunner()
    {
        var target = GameBuildSettings.HostPlatform == GameBuildPlatform.Linux ? GameBuildPlatform.Windows : GameBuildPlatform.Linux;
        var toolchain = Path.Combine(root, "cross.cmake");
        File.WriteAllText(toolchain, "toolchain fixture");
        runner.ProbeSystem = "WrongSystem";
        var mismatch = await GameBuildEnvironment.CheckAsync(Workspace, target, toolchain, runner);
        Assert.False(mismatch.CanBuild);
        Assert.Contains("WrongSystem", mismatch.Detail);
        runner.Platform = target;
        runner.ProbeSystem = null;
        var noRunner = await GameBuildEnvironment.CheckAsync(Workspace, target, toolchain, runner);
        Assert.False(noRunner.CanBuild);
        Assert.Contains("CMAKE_CROSSCOMPILING_EMULATOR", noRunner.Detail);
        runner.TargetRunner = Path.Combine(root, "missing runner");
        var missingRunner = await GameBuildEnvironment.CheckAsync(Workspace, target, toolchain, runner);
        Assert.False(missingRunner.CanBuild);
        Assert.Contains("not installed", missingRunner.Detail);
    }

    [Fact]
    public void NativeBuildDiscoveryReusesOnlyTheMatchingEngineAndConfiguration()
    {
        var build = Path.Combine(Engine, "build-current");
        var system = Path.Combine(build, "CMakeFiles", "4.2.0");
        Directory.CreateDirectory(system);
        File.WriteAllText(Path.Combine(build, "CMakeCache.txt"),
            "CMAKE_HOME_DIRECTORY:INTERNAL=" + Engine + "\nCMAKE_BUILD_TYPE:STRING=Release\n");
        var systemName = GameBuildSettings.HostPlatform switch
        {
            GameBuildPlatform.Windows => "Windows",
            GameBuildPlatform.Linux => "Linux",
            _ => "Darwin"
        };
        File.WriteAllText(Path.Combine(system, "CMakeSystem.cmake"), $"set(CMAKE_SYSTEM_NAME \"{systemName}\")\n");
        Assert.Equal(build, GameBuildEnvironment.FindEngineBuild(Engine, GameBuildSettings.HostPlatform, "Release"));
        Assert.NotEqual(build, GameBuildEnvironment.FindEngineBuild(Engine, GameBuildSettings.HostPlatform, "Debug"));
        File.WriteAllText(Path.Combine(build, "CMakeCache.txt"),
            "CMAKE_HOME_DIRECTORY:INTERNAL=" + Workspace + "\nCMAKE_BUILD_TYPE:STRING=Release\n");
        Assert.NotEqual(build, GameBuildEnvironment.FindEngineBuild(Engine, GameBuildSettings.HostPlatform, "Release"));
    }

    public void Dispose() => Directory.Delete(root, recursive: true);

    sealed class FakeRunner(string workspace, string engine) : IWorkspaceProcessRunner
    {
        public List<WorkspaceProcessInvocation> Invocations { get; } = [];
        public GameBuildPlatform Platform { get; set; } = GameBuildSettings.HostPlatform;
        public string? TargetRunner { get; set; }
        public string? ProbeSystem { get; set; }
        public bool FailArchive { get; set; }
        public string? FailCommand { get; set; }
        public Action? BeforeArchive { get; set; }
        public static string ModuleName => OperatingSystem.IsWindows() ? "Game.dll" : OperatingSystem.IsMacOS() ? "libGame.dylib" : "libGame.so";

        public Task<WorkspaceProcessResult> RunAsync(WorkspaceProcessInvocation invocation, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Invocations.Add(invocation);
            var arguments = invocation.FileName == TargetRunner ? invocation.Arguments.Skip(1).ToArray() : invocation.Arguments;
            if (arguments[0] == "-S" && arguments[1].Contains("BuildToolchains"))
            {
                var build = arguments[arguments.ToList().IndexOf("-B") + 1];
                Directory.CreateDirectory(build);
                File.WriteAllText(Path.Combine(build, "Target.txt"), $"system={ProbeSystem ?? (Platform == GameBuildPlatform.Windows ? "Windows" : Platform == GameBuildPlatform.MacOS ? "Darwin" : "Linux")}\nprocessor=test-arch\nrunner={TargetRunner}\n");
            }
            if (arguments[0] == FailCommand)
            {
                return Task.FromResult(new WorkspaceProcessResult(1, "tool validation failed", TimeSpan.Zero));
            }
            string output = "";
            if (arguments[0] == "--describe-workspace")
            {
                output = JsonSerializer.Serialize(new { schemaVersion = 1, legacy = false, engineMode = false,
                    workspaceId = "workspace-id", name = "Project Я", content = Path.Combine(workspace, "Content"),
                    engineContent = Path.Combine(engine, "Content"), engineRoot = engine, generatedProject = Path.Combine(workspace, "Generated"),
                    cache = Path.Combine(workspace, "Cache"), logicOutput = Path.Combine(workspace, "Binaries"), logicModuleName = "Game" });
            }
            else if (arguments[0] == "--build")
            {
                var project = Path.GetDirectoryName(arguments[1])!;
                var configuration = arguments[arguments.ToList().IndexOf("--config") + 1];
                var target = Path.Combine(project, "Binaries", configuration);
                Directory.CreateDirectory(target);
                File.WriteAllText(Path.Combine(target, Platform switch
                {
                    GameBuildPlatform.Windows => "Game.dll",
                    GameBuildPlatform.MacOS => "libGame.dylib",
                    _ => "libGame.so"
                }), "module");
                if (Platform == GameBuildPlatform.Windows)
                {
                    File.WriteAllText(Path.Combine(project, "runtime-dlls-" + configuration + ".txt"), "");
                }
            }
            else if (arguments[0] == "--copy-assets")
            {
                Directory.CreateDirectory(arguments[2]);
                foreach (var file in Directory.EnumerateFiles(arguments[1], "*", SearchOption.AllDirectories))
                {
                    var target = Path.Combine(arguments[2], Path.GetRelativePath(arguments[1], file));
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    File.Copy(file, target);
                }
            }
            else if (arguments[0] is "--pack-assets" or "--pack-cache")
            {
                BeforeArchive?.Invoke();
                cancellationToken.ThrowIfCancellationRequested();
                if (FailArchive)
                {
                    return Task.FromResult(new WorkspaceProcessResult(1, "archive failure", TimeSpan.Zero));
                }
                if (arguments[0] == "--pack-assets")
                {
                    ZipFile.CreateFromDirectory(arguments[1], arguments[2]);
                }
                else
                {
                    File.WriteAllText(arguments[2], "seed fixture");
                }
            }
            return Task.FromResult(new WorkspaceProcessResult(0, output, TimeSpan.Zero));
        }
    }
}
