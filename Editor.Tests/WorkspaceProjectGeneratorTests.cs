using System.Diagnostics;
using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceProjectGeneratorTests
{
    [Fact]
    public async Task GenerateAsync_CreatesSourceEngineProjectWithExpectedLayout()
    {
        using var workspace = TempWorkspace.Create(includeSpaces: true);
        using var engine = TempWorkspace.Create(includeSpaces: true);
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", engine.Root, "workspace-id");
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        await generator.GenerateAsync(session);

        var cmake = await File.ReadAllTextAsync(workspace.File("Generated/CMakeLists.txt"));
        Assert.Contains($"add_subdirectory(\"{ToCMakePath(engine.Root)}\" \"${{CMAKE_BINARY_DIR}}/SailorEngine\" EXCLUDE_FROM_ALL)", cmake);
        Assert.Contains("set(SAILOR_BUILD_EXECUTABLE OFF", cmake);
        Assert.Contains("set(SAILOR_BUILD_TESTS OFF", cmake);
        Assert.Contains("target_link_libraries(SailorGame PRIVATE Sailor::Runtime)", cmake);
        Assert.Contains("WINDOWS_EXPORT_ALL_SYMBOLS ON", cmake);
        Assert.Contains("if(NOT WIN32)", cmake);
        Assert.Contains("if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)", cmake);
        Assert.Contains("CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL \"MSVC\"", cmake);
        Assert.Contains("PDB_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"", cmake);
        Assert.Contains("COMPILE_PDB_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Source", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Binaries", cmake);
        Assert.True(File.Exists(workspace.File("Source/Components/SampleComponent.h")));
        Assert.True(File.Exists(workspace.File("Source/Components/SampleComponent.cpp")));
        Assert.True(File.Exists(workspace.File(".gitignore")));
        Assert.True(Directory.Exists(workspace.Directory("Cache/Build")));
        Assert.True(Directory.Exists(workspace.Directory("Binaries")));
    }

    [Fact]
    public async Task GenerateAsync_CreatesInstalledEngineProjectWithCustomPaths()
    {
        using var workspace = TempWorkspace.Create(includeSpaces: true);
        using var engineInstall = TempWorkspace.Create(includeSpaces: true);
        var manifest = WorkspaceManifest.CreateDefault(
            "Sandbox",
            engineInstall.Root,
            "workspace-id",
            WorkspaceEngineReferenceKinds.Installed) with
        {
            SourcePath = "Game Source",
            GeneratedProjectPath = "Project Files",
            BuildPath = "Intermediate/Build Files",
            LogicOutputPath = "Output Files",
            LogicModuleName = "GameLogic"
        };
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        await generator.GenerateAsync(session);

        var cmake = await File.ReadAllTextAsync(workspace.File("Project Files/CMakeLists.txt"));
        Assert.Contains($"list(PREPEND CMAKE_PREFIX_PATH \"{ToCMakePath(engineInstall.Root)}\")", cmake);
        Assert.Contains("find_package(Sailor CONFIG REQUIRED)", cmake);
        Assert.DoesNotContain("add_subdirectory", cmake);
        Assert.Contains("project(GameLogic LANGUAGES CXX)", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Game Source", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Output Files", cmake);
        Assert.True(Directory.Exists(workspace.Directory("Intermediate/Build Files")));
        Assert.True(Directory.Exists(workspace.Directory("Output Files")));
    }

    [Fact]
    public async Task GenerateAsync_DoesNotOverwriteAnyExistingProjectFile()
    {
        using var workspace = TempWorkspace.Create();
        var componentsDirectory = workspace.Directory("Source/Components");
        Directory.CreateDirectory(componentsDirectory);
        var existingHeader = workspace.File("Source/Components/SampleComponent.h");
        await File.WriteAllTextAsync(existingHeader, "user-owned content");
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => generator.GenerateAsync(session));

        Assert.Contains("will not be overwritten", error.Message, StringComparison.Ordinal);
        Assert.Equal("user-owned content", await File.ReadAllTextAsync(existingHeader));
        Assert.False(File.Exists(workspace.File(".gitignore")));
        Assert.False(File.Exists(workspace.File("Generated/CMakeLists.txt")));
    }

    [Fact]
    public async Task GenerateAsync_EmitsEditableSampleComponent()
    {
        using var workspace = TempWorkspace.Create();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            LogicModuleName = "SandboxLogic"
        };
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        await generator.GenerateAsync(session);

        var header = await File.ReadAllTextAsync(workspace.File("Source/Components/SampleComponent.h"));
        var source = await File.ReadAllTextAsync(workspace.File("Source/Components/SampleComponent.cpp"));
        Assert.Contains("namespace SandboxLogic", header);
        Assert.Contains("class SampleComponent final : public Sailor::Component", header);
        Assert.Contains("type(SandboxLogic::SampleComponent, bases<Sailor::Component>)", header);
        Assert.Contains("void SandboxLogic::SampleComponent::BeginPlay()", source);
    }

    static WorkspaceSession CreateSession(string workspaceRoot, WorkspaceManifest manifest)
    {
        var serializer = new WorkspaceManifestSerializer();
        return new WorkspaceTemplateService(serializer).CreateSession(workspaceRoot, manifest);
    }

    static string ToCMakePath(string path)
        => Path.GetFullPath(path).Replace('\\', '/');

    sealed class TempWorkspace : IDisposable
    {
        TempWorkspace(string root)
        {
            Root = root;
            System.IO.Directory.CreateDirectory(root);
        }

        public string Root { get; }

        public static TempWorkspace Create(bool includeSpaces = false)
        {
            var prefix = includeSpaces ? "sailor workspace" : "sailor-workspace";
            return new TempWorkspace(Path.Combine(Path.GetTempPath(), $"{prefix}-{Guid.NewGuid():N}"));
        }

        public string File(string relativePath) => Path.Combine(Root, relativePath);

        public string Directory(string relativePath) => Path.Combine(Root, relativePath);

        public void Dispose()
        {
            if (System.IO.Directory.Exists(Root))
                System.IO.Directory.Delete(Root, recursive: true);
        }
    }
}

public sealed class WorkspaceProjectGeneratorIntegrationTests
{
    [Fact]
    [Trait("Category", "CMakeIntegration")]
    public async Task SourceMode_ConfiguresBuildsAndEmitsReleaseDll()
    {
        if (!ShouldRun)
            return;

        EnsureWindows();
        var engineRoot = GetRequiredEnvironmentVariable("SAILOR_ENGINE_SOURCE_DIR");
        await GenerateBuildAndAssertAsync(
            engineRoot,
            WorkspaceEngineReferenceKinds.Source,
            "SailorSourceGame");
    }

    [Fact]
    [Trait("Category", "CMakeIntegration")]
    public async Task InstalledMode_ConfiguresBuildsAndEmitsReleaseDll()
    {
        if (!ShouldRun)
            return;

        EnsureWindows();
        var installPrefix = GetRequiredEnvironmentVariable("SAILOR_ENGINE_INSTALL_PREFIX");
        await GenerateBuildAndAssertAsync(
            installPrefix,
            WorkspaceEngineReferenceKinds.Installed,
            "SailorInstalledGame");
    }

    static bool ShouldRun
        => string.Equals(
            Environment.GetEnvironmentVariable("SAILOR_RUN_CMAKE_INTEGRATION"),
            "1",
            StringComparison.Ordinal);

    static async Task GenerateBuildAndAssertAsync(
        string enginePath,
        string engineReferenceKind,
        string moduleName)
    {
        using var workspace = IntegrationWorkspace.Create();
        var manifest = WorkspaceManifest.CreateDefault(
            "Integration",
            Path.GetFullPath(enginePath),
            "integration-workspace",
            engineReferenceKind) with
        {
            LogicModuleName = moduleName
        };
        var serializer = new WorkspaceManifestSerializer();
        var session = new WorkspaceTemplateService(serializer).CreateSession(workspace.Root, manifest);
        await new WorkspaceProjectGenerator().GenerateAsync(session);

        var configureArguments = new List<string>
        {
            "-S", session.GeneratedProjectDirectory,
            "-B", session.BuildDirectory
        };
        AddEnvironmentOption(configureArguments, "-G", "SAILOR_CMAKE_GENERATOR");
        AddEnvironmentOption(configureArguments, "-A", "SAILOR_CMAKE_ARCHITECTURE");
        AddEnvironmentOption(configureArguments, "-T", "SAILOR_CMAKE_TOOLSET");

        var toolchainFile = Environment.GetEnvironmentVariable("SAILOR_CMAKE_TOOLCHAIN_FILE");
        if (!string.IsNullOrWhiteSpace(toolchainFile))
            configureArguments.Add($"-DCMAKE_TOOLCHAIN_FILE={Path.GetFullPath(toolchainFile)}");

        await RunCMakeAsync(session.WorkspaceRoot, configureArguments);
        await RunCMakeAsync(session.WorkspaceRoot, [
            "--build", session.BuildDirectory,
            "--config", "Release",
            "--target", moduleName,
            "--parallel"
        ]);

        var expectedDll = Path.Combine(session.LogicOutputDirectory, "Release", $"{moduleName}.dll");
        var expectedImportLibrary = Path.Combine(session.LogicOutputDirectory, "Release", $"{moduleName}.lib");
        Assert.True(File.Exists(expectedDll), $"Expected generated logic module was not found: {expectedDll}");
        Assert.True(
            File.Exists(expectedImportLibrary),
            $"Expected generated logic import library was not found: {expectedImportLibrary}");
    }

    static void AddEnvironmentOption(ICollection<string> arguments, string option, string variableName)
    {
        var value = Environment.GetEnvironmentVariable(variableName);
        if (string.IsNullOrWhiteSpace(value))
            return;

        arguments.Add(option);
        arguments.Add(value);
    }

    static async Task RunCMakeAsync(string workingDirectory, IEnumerable<string> arguments)
    {
        var executable = Environment.GetEnvironmentVariable("SAILOR_CMAKE_EXE");
        var startInfo = new ProcessStartInfo
        {
            FileName = string.IsNullOrWhiteSpace(executable) ? "cmake" : executable,
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };

        foreach (var argument in arguments)
            startInfo.ArgumentList.Add(argument);

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Failed to start CMake executable: {startInfo.FileName}");
        var outputTask = process.StandardOutput.ReadToEndAsync();
        var errorTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        var output = await outputTask;
        var error = await errorTask;

        Assert.True(
            process.ExitCode == 0,
            $"CMake exited with code {process.ExitCode}.\nArguments: {string.Join(" ", startInfo.ArgumentList)}\n{output}\n{error}");
    }

    static string GetRequiredEnvironmentVariable(string name)
    {
        var value = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"{name} is required when SAILOR_RUN_CMAKE_INTEGRATION=1.");

        return value;
    }

    static void EnsureWindows()
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("Workspace CMake integration tests require Windows.");
    }

    sealed class IntegrationWorkspace : IDisposable
    {
        IntegrationWorkspace(string root)
        {
            Root = root;
            Directory.CreateDirectory(root);
        }

        public string Root { get; }

        public static IntegrationWorkspace Create()
            => new(Path.Combine(Path.GetTempPath(), $"sailor cmake integration-{Guid.NewGuid():N}"));

        public void Dispose()
        {
            if (Directory.Exists(Root))
                Directory.Delete(Root, recursive: true);
        }
    }
}
