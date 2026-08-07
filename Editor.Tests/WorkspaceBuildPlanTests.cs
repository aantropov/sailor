using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class WorkspaceBuildPlanTests
{
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
        var repositoryRoot = ResolveRepositoryRoot();
        var root = Path.Combine(Path.GetTempPath(), "Sailor Workspace");
        var session = new WorkspaceSession(
            root,
            Path.Combine(root, "workspace.sailor"),
            WorkspaceManifest.CreateDefault("Game", repositoryRoot),
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
            "Release",
            configure: true);

        var toolchainPath = Path.Combine(
            repositoryRoot,
            "External",
            "vcpkg",
            "scripts",
            "buildsystems",
            "vcpkg.cmake");
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
            var installedDirectory = Path.Combine(
                repositoryRoot,
                "External",
                "vcpkg",
                "installed",
                triplet);
            if (Directory.Exists(installedDirectory))
            {
                Assert.Contains(
                    "-DCMAKE_PREFIX_PATH=" + installedDirectory,
                    plan.Invocations[0].Arguments);
            }
        }
    }

    static string ResolveRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "CMakeLists.txt")) &&
                Directory.Exists(Path.Combine(current.FullName, "Editor")))
            {
                return current.FullName;
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
