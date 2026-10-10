using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class WorkspaceBuildPlanTests
{
    [Fact]
    public void Compile_ReconfiguresForTheRunningEditorInsteadOfTheManifestEngine()
    {
        var root = Directory.CreateTempSubdirectory("sailor-active-engine-");
        try
        {
            var activeEngine = Path.Combine(root.FullName, "Current engine");
            Directory.CreateDirectory(Path.Combine(activeEngine, "Runtime"));
            var session = new WorkspaceSession(root.FullName, Path.Combine(root.FullName, "Game.sailor"),
                WorkspaceManifest.CreateDefault("Game", "../OldEngine"),
                Path.Combine(root.FullName, "Content"), Path.Combine(root.FullName, "Source"),
                Path.Combine(root.FullName, "Generated"), Path.Combine(root.FullName, "Cache"))
            {
                BuildDirectory = Path.Combine(root.FullName, "Cache", "Build"),
                LogicOutputDirectory = Path.Combine(root.FullName, "Binaries"),
            };

            var plan = WorkspaceBuildPlan.Create(session, "Release", configure: false,
                engineDirectory: activeEngine);

            Assert.Equal(2, plan.Invocations.Count);
            Assert.Contains("-DSAILOR_ENGINE_ROOT=" + activeEngine, plan.Invocations[0].Arguments);
            Assert.Contains("-DSAILOR_ENGINE_REFERENCE_KIND=source", plan.Invocations[0].Arguments);
            Assert.Contains("--build", plan.Invocations[1].Arguments);
            Assert.Equal(session.WorkspaceRoot, plan.Invocations[1].WorkingDirectory);

            var sdk = Path.Combine(root.FullName, "Editor", "EngineSDK");
            var sdkPlan = WorkspaceBuildPlan.Create(session, "Release", configure: false,
                engineDirectory: activeEngine, engineSdkDirectory: sdk);
            Assert.Equal(2, sdkPlan.Invocations.Count);
            Assert.Contains("-DSAILOR_ENGINE_ROOT=" + sdk, sdkPlan.Invocations[0].Arguments);
            Assert.Contains("-DSAILOR_ENGINE_REFERENCE_KIND=installed", sdkPlan.Invocations[0].Arguments);
            Assert.Contains("-DSailor_DIR=" + Path.Combine(sdk, "lib", "cmake", "Sailor"), sdkPlan.Invocations[0].Arguments);
        }
        finally
        {
            root.Delete(recursive: true);
        }
    }

    [Fact]
    public void CreateConfigure_OnlyCreatesCMakeProjectInvocation()
    {
        var root = Path.Combine(Path.GetTempPath(), "Sailor Workspace");
        var session = new WorkspaceSession(
            root,
            Path.Combine(root, "workspace.sailor"),
            WorkspaceManifest.CreateDefault("Game", root),
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            Path.Combine(root, "Cache"))
        {
            BuildDirectory = Path.Combine(root, "Cache", "Build"),
            LogicOutputDirectory = Path.Combine(root, "Binaries"),
        };

        var plan = WorkspaceBuildPlan.CreateConfigure(session, "Release");

        Assert.Single(plan.Invocations);
        Assert.Equal("-S", plan.Invocations[0].Arguments[0]);
        Assert.DoesNotContain("--build", plan.Invocations[0].Arguments);
    }

    [Fact]
    public void Create_BuildsConfigureAndTargetInvocationsWithoutShellQuoting()
    {
        var root = Path.Combine(Path.GetTempPath(), "Sailor Workspace");
        var session = new WorkspaceSession(
            root,
            Path.Combine(root, "workspace.sailor"),
            WorkspaceManifest.CreateDefault("Game", root) with
            {
                LogicModuleName = "ForestGame",
            },
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            Path.Combine(root, "Cache"))
        {
            BuildDirectory = Path.Combine(root, "Cache", "Build"),
            LogicOutputDirectory = Path.Combine(root, "Binaries"),
        };

        var plan = WorkspaceBuildPlan.Create(
            session,
            "RelWithDebInfo",
            configure: true);

        Assert.Equal("RelWithDebInfo", plan.Configuration);
        Assert.Equal(2, plan.Invocations.Count);
        Assert.Equal(
            [
                "-S",
                session.GeneratedProjectDirectory,
                "-B",
                session.BuildDirectory,
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            ],
            plan.Invocations[0].Arguments);
        Assert.Equal(
            [
                "--build",
                session.BuildDirectory,
                "--config",
                "RelWithDebInfo",
                "--target",
                "ForestGame",
                "--parallel",
                "4",
            ],
            plan.Invocations[1].Arguments);
    }

    [Fact]
    public void Create_RejectsUnknownConfiguration()
    {
        var root = Path.GetTempPath();
        var session = new WorkspaceSession(
            root,
            Path.Combine(root, "workspace.sailor"),
            WorkspaceManifest.CreateDefault("Game", root),
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            Path.Combine(root, "Cache"))
        {
            BuildDirectory = Path.Combine(root, "Cache", "Build"),
            LogicOutputDirectory = Path.Combine(root, "Binaries"),
        };

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            WorkspaceBuildPlan.Create(
                session,
                "Shipping",
                configure: false));
    }

    [Fact]
    public void Create_UsesSourceEngineVcpkgToolchainWhenAvailable()
    {
        var directory = Directory.CreateTempSubdirectory("Sailor Workspace Build ");
        try
        {
            var root = directory.FullName;
            var engineRoot = Path.Combine(root, "Engine");
            var session = new WorkspaceSession(
                root,
                Path.Combine(root, "workspace.sailor"),
                WorkspaceManifest.CreateDefault("Game", engineRoot),
                Path.Combine(root, "Content"),
                Path.Combine(root, "Source"),
                Path.Combine(root, "Generated"),
                Path.Combine(root, "Cache"))
            {
                BuildDirectory = Path.Combine(root, "Cache", "Build"),
                LogicOutputDirectory = Path.Combine(root, "Binaries"),
            };
            var plan = WorkspaceBuildPlan.Create(session, "Release", configure: true);
            Assert.Equal(
                ["-S", session.GeneratedProjectDirectory, "-B", session.BuildDirectory, "-DCMAKE_BUILD_TYPE=Release"],
                plan.Invocations[0].Arguments);

            var toolchainPath = Path.Combine(engineRoot, "External", "vcpkg", "scripts", "buildsystems", "vcpkg.cmake");
            Directory.CreateDirectory(Path.GetDirectoryName(toolchainPath)!);
            File.WriteAllText(toolchainPath, "# Build-plan fixture; CMake is not invoked.\n");
            plan = WorkspaceBuildPlan.Create(session, "Release", configure: true);
            Assert.Contains(
                "-DCMAKE_TOOLCHAIN_FILE=" + toolchainPath,
                plan.Invocations[0].Arguments);
            var triplet = OperatingSystem.IsMacOS() || OperatingSystem.IsMacCatalyst()
                ? "arm64-osx"
                : OperatingSystem.IsWindows()
                    ? "x64-windows"
                    : OperatingSystem.IsLinux()
                        ? "x64-linux"
                        : null;
            if (triplet is not null)
            {
                Assert.Contains(
                    "-DVCPKG_TARGET_TRIPLET=" + triplet,
                    plan.Invocations[0].Arguments);
                var installedDirectory = Path.Combine(engineRoot, "External", "vcpkg", "installed", triplet);
                var stbModules = Path.Combine(installedDirectory, "share", "stb");
                Assert.DoesNotContain("-DCMAKE_PREFIX_PATH=" + installedDirectory, plan.Invocations[0].Arguments);
                Assert.DoesNotContain("-DCMAKE_MODULE_PATH=" + stbModules, plan.Invocations[0].Arguments);
                Directory.CreateDirectory(stbModules);
                plan = WorkspaceBuildPlan.Create(session, "Release", configure: true);
                Assert.Contains("-DCMAKE_PREFIX_PATH=" + installedDirectory, plan.Invocations[0].Arguments);
                Assert.Contains("-DCMAKE_MODULE_PATH=" + stbModules, plan.Invocations[0].Arguments);
            }
        }
        finally
        {
            directory.Delete(recursive: true);
        }
    }
}
