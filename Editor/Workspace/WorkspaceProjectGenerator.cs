using System.Text;

namespace SailorEditor.Workspace;

public sealed record WorkspaceProjectGenerationResult(
    IReadOnlyList<string> CreatedFiles,
    IReadOnlyList<string> CreatedDirectories);

public sealed class WorkspaceProjectGenerator
{
    public async Task<WorkspaceProjectGenerationResult> GenerateAsync(
        WorkspaceSession session,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(session);

        var componentsDirectory = Path.Combine(session.SourceDirectory, "Components");
        var generatedFiles = new Dictionary<string, string>
        {
            [Path.Combine(session.WorkspaceRoot, ".gitignore")] = BuildGitIgnore(session.Manifest),
            [Path.Combine(session.GeneratedProjectDirectory, "CMakeLists.txt")] = BuildCMakeProject(session),
            [Path.Combine(componentsDirectory, "SampleComponent.h")] = BuildSampleComponentHeader(session.Manifest.LogicModuleName),
            [Path.Combine(componentsDirectory, "SampleComponent.cpp")] = BuildSampleComponentSource(session.Manifest.LogicModuleName),
            [Path.Combine(session.SourceDirectory, "WorkspaceTypes.h")] = BuildWorkspaceTypesHeader(session.Manifest.LogicModuleName),
            [Path.Combine(session.SourceDirectory, "WorkspaceModule.cpp")] = BuildWorkspaceModuleSource(session.Manifest.LogicModuleName)
        };

        foreach (var path in generatedFiles.Keys)
        {
            EnsureInsideWorkspace(session.WorkspaceRoot, path);
            if (File.Exists(path) || Directory.Exists(path))
                throw new InvalidOperationException($"Workspace project file already exists and will not be overwritten: {path}");
        }

        var createdFiles = new List<string>();
        var createdDirectories = new List<string>();

        try
        {
            foreach (var directory in GetProjectDirectories(session, componentsDirectory))
            {
                EnsureInsideWorkspace(session.WorkspaceRoot, directory);
                CreateDirectoryTracked(session.WorkspaceRoot, directory, createdDirectories);
            }

            foreach (var (path, content) in generatedFiles)
                await WriteNewFileAsync(path, content, createdFiles, cancellationToken);

            return new WorkspaceProjectGenerationResult(createdFiles.ToArray(), createdDirectories.ToArray());
        }
        catch (Exception generationError)
        {
            try
            {
                Rollback(new WorkspaceProjectGenerationResult(createdFiles, createdDirectories));
            }
            catch (Exception rollbackError)
            {
                throw new AggregateException("Workspace project generation failed and could not be rolled back.", generationError, rollbackError);
            }

            throw;
        }
    }

    static IEnumerable<string> GetProjectDirectories(WorkspaceSession session, string componentsDirectory)
    {
        yield return session.ContentDirectory;
        yield return session.SourceDirectory;
        yield return componentsDirectory;
        yield return session.GeneratedProjectDirectory;
        yield return session.CacheDirectory;
        yield return session.BuildDirectory;
        yield return session.LogicOutputDirectory;
    }

    static string BuildCMakeProject(WorkspaceSession session)
    {
        var moduleName = session.Manifest.LogicModuleName;
        var sourcePath = BuildWorkspaceRelativeCMakePath(session.GeneratedProjectDirectory, session.SourceDirectory);
        var outputPath = BuildWorkspaceRelativeCMakePath(session.GeneratedProjectDirectory, session.LogicOutputDirectory);
        var enginePath = BuildEngineCMakePath(session);
        var lines = new List<string>
        {
            "cmake_minimum_required(VERSION 3.21)",
            $"project({moduleName} LANGUAGES CXX)",
            string.Empty,
            "if(NOT WIN32)",
            "  message(FATAL_ERROR \"Sailor user logic currently supports Windows only.\")",
            "endif()",
            string.Empty,
            "if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)",
            "  message(FATAL_ERROR \"Sailor user logic requires a 64-bit toolchain.\")",
            "endif()",
            string.Empty,
            "if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL \"MSVC\" OR",
            "        (CMAKE_CXX_COMPILER_ID MATCHES \"Clang\" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL \"MSVC\")))",
            "  message(FATAL_ERROR \"Sailor user logic requires MSVC or clang-cl.\")",
            "endif()",
            string.Empty,
            "if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)",
            "  set(CMAKE_BUILD_TYPE Release CACHE STRING \"Build type\" FORCE)",
            "endif()",
            string.Empty,
            $"set(SAILOR_GAME_SOURCE_DIR \"{sourcePath}\")",
            $"set(SAILOR_GAME_OUTPUT_DIR \"{outputPath}\")",
            string.Empty
        };

        if (session.Manifest.EngineReferenceKind == WorkspaceEngineReferenceKinds.Source)
        {
            lines.AddRange([
                "set(SAILOR_BUILD_EXECUTABLE OFF CACHE BOOL \"Build Sailor executables\" FORCE)",
                "set(SAILOR_BUILD_TESTS OFF CACHE BOOL \"Build Sailor tests\" FORCE)",
                $"add_subdirectory(\"{enginePath}\" \"${{CMAKE_BINARY_DIR}}/SailorEngine\" EXCLUDE_FROM_ALL)"
            ]);
        }
        else if (session.Manifest.EngineReferenceKind == WorkspaceEngineReferenceKinds.Installed)
        {
            lines.AddRange([
                $"list(PREPEND CMAKE_PREFIX_PATH \"{enginePath}\")",
                "find_package(Sailor CONFIG REQUIRED)"
            ]);
        }
        else
        {
            throw new InvalidOperationException($"Unsupported engine reference kind: {session.Manifest.EngineReferenceKind}");
        }

        lines.AddRange([
            string.Empty,
            "file(GLOB_RECURSE SAILOR_GAME_SOURCES CONFIGURE_DEPENDS",
            "  \"${SAILOR_GAME_SOURCE_DIR}/*.h\"",
            "  \"${SAILOR_GAME_SOURCE_DIR}/*.hpp\"",
            "  \"${SAILOR_GAME_SOURCE_DIR}/*.cpp\")",
            string.Empty,
            $"add_library({moduleName} SHARED ${{SAILOR_GAME_SOURCES}})",
            $"target_include_directories({moduleName} PRIVATE \"${{SAILOR_GAME_SOURCE_DIR}}\")",
            $"target_link_libraries({moduleName} PRIVATE Sailor::Runtime)",
            $"target_compile_features({moduleName} PRIVATE cxx_std_20)",
            $"target_compile_definitions({moduleName} PRIVATE",
            "  SAILOR_WORKSPACE_MODULE",
            "  SAILOR_WORKSPACE_MODULE_EXPORTS)",
            string.Empty,
            $"set_target_properties({moduleName} PROPERTIES",
            $"  OUTPUT_NAME \"{moduleName}\"",
            "  RUNTIME_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"",
            "  LIBRARY_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"",
            "  ARCHIVE_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"",
            "  PDB_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"",
            "  COMPILE_PDB_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\")",
            string.Empty
        ]);

        return string.Join("\n", lines);
    }

    static string BuildWorkspaceRelativeCMakePath(string projectDirectory, string targetDirectory)
    {
        var relativePath = Path.GetRelativePath(projectDirectory, targetDirectory);
        if (relativePath == ".")
            return "${CMAKE_CURRENT_LIST_DIR}";

        return $"${{CMAKE_CURRENT_LIST_DIR}}/{EscapeCMakePath(relativePath)}";
    }

    static string BuildEngineCMakePath(WorkspaceSession session)
    {
        var manifestPath = session.Manifest.EnginePath
            .Replace('/', Path.DirectorySeparatorChar)
            .Replace('\\', Path.DirectorySeparatorChar);

        if (Path.IsPathRooted(manifestPath))
            return EscapeCMakePath(Path.GetFullPath(manifestPath));

        var engineDirectory = Path.GetFullPath(Path.Combine(session.WorkspaceRoot, manifestPath));
        return BuildWorkspaceRelativeCMakePath(session.GeneratedProjectDirectory, engineDirectory);
    }

    static string EscapeCMakePath(string path)
        => path
            .Replace('\\', '/')
            .Replace("$", "\\$", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace(";", "\\;", StringComparison.Ordinal);

    static string BuildSampleComponentHeader(string moduleName)
        => string.Join("\n", [
            "#pragma once",
            string.Empty,
            "#include \"Components/Component.h\"",
            string.Empty,
            $"namespace {moduleName}",
            "{",
            "\tusing namespace Sailor;",
            string.Empty,
            "\tclass SampleComponent final : public Sailor::Component",
            "\t{",
            "\t\tSAILOR_WORKSPACE_REFLECTABLE(SampleComponent)",
            string.Empty,
            "\tpublic:",
            "\t\tvoid BeginPlay() override;",
            "\t\tfloat GetMoveSpeed() const { return m_moveSpeed; }",
            "\t\tvoid SetMoveSpeed(float moveSpeed) { m_moveSpeed = moveSpeed; }",
            string.Empty,
            "\tprivate:",
            "\t\tfloat m_moveSpeed = 5.0f;",
            "\t};",
            "}",
            string.Empty,
            "REFL_AUTO(",
            $"\ttype({moduleName}::SampleComponent, bases<Sailor::Component>),",
            "\tfunc(GetMoveSpeed, property(\"moveSpeed\")),",
            "\tfunc(SetMoveSpeed, property(\"moveSpeed\"))",
            ")",
            string.Empty
        ]);

    static string BuildWorkspaceTypesHeader(string moduleName)
        => string.Join("\n", [
            "#pragma once",
            string.Empty,
            "#include \"Components/SampleComponent.h\"",
            "#include \"Workspace/WorkspaceTypeMetadata.h\"",
            string.Empty,
            $"namespace {moduleName}",
            "{",
            "\tusing WorkspaceTypes = Sailor::Workspace::TWorkspaceTypeList<SampleComponent>;",
            "}",
            string.Empty
        ]);

    static string BuildWorkspaceModuleSource(string moduleName)
        => string.Join("\n", [
            "#include \"WorkspaceTypes.h\"",
            "#include \"Workspace/WorkspaceTypeRegistration.h\"",
            string.Empty,
            "namespace",
            "{",
            $"\tconstexpr char WorkspaceModuleName[] = \"{moduleName}\";",
            string.Empty,
            "\tuint32_t SAILOR_WORKSPACE_CALL RegisterWorkspaceTypes(",
            "\t\tconst Sailor::Workspace::WorkspaceHostApiV1* hostApi) noexcept",
            "\t{",
            $"\t\treturn Sailor::Workspace::RegisterWorkspaceTypesV1<{moduleName}::WorkspaceTypes>(hostApi);",
            "\t}",
            "}",
            string.Empty,
            "extern \"C\" SAILOR_WORKSPACE_MODULE_EXPORT uint32_t SAILOR_WORKSPACE_CALL SailorGetWorkspaceTypeMetadataV1(",
            "\tchar* destination,",
            "\tuint64_t destinationCapacity,",
            "\tuint64_t* outPayloadSize) noexcept",
            "{",
            $"\treturn Sailor::Workspace::ExportWorkspaceTypeMetadataV1<{moduleName}::WorkspaceTypes>(",
            "\t\tWorkspaceModuleName, destination, destinationCapacity, outPayloadSize);",
            "}",
            string.Empty,
            "extern \"C\" SAILOR_WORKSPACE_MODULE_EXPORT const Sailor::Workspace::WorkspaceModuleApiV1* SAILOR_WORKSPACE_CALL",
            "\tSailorGetWorkspaceModuleApiV1() noexcept",
            "{",
            "\tstatic const Sailor::Workspace::WorkspaceModuleApiV1 api",
            "\t{",
            "\t\tstatic_cast<uint32_t>(sizeof(Sailor::Workspace::WorkspaceModuleApiV1)),",
            "\t\tSailor::Workspace::WorkspaceModuleApiVersion,",
            "\t\tWorkspaceModuleName,",
            "\t\tstatic_cast<uint64_t>(sizeof(WorkspaceModuleName) - 1),",
            "\t\tSailor::Workspace::GetWorkspaceModuleAbiTagV1(),",
            "\t\tSailor::Workspace::GetWorkspaceModuleAbiTagV1Length(),",
            "\t\t&SailorGetWorkspaceTypeMetadataV1,",
            "\t\t&RegisterWorkspaceTypes",
            "\t};",
            string.Empty,
            "\treturn &api;",
            "}",
            string.Empty
        ]);

    static string BuildSampleComponentSource(string moduleName)
        => string.Join("\n", [
            "#include \"Components/SampleComponent.h\"",
            string.Empty,
            $"void {moduleName}::SampleComponent::BeginPlay()",
            "{",
            "}",
            string.Empty
        ]);

    static string BuildGitIgnore(WorkspaceManifest manifest)
    {
        var ignoredPaths = new[] { manifest.CachePath, manifest.BuildPath, manifest.LogicOutputPath }
            .Select(WorkspaceManifestPaths.Normalize)
            .Where(x => !string.IsNullOrWhiteSpace(x))
            .Select(x => $"/{x}/")
            .Distinct(StringComparer.Ordinal);

        return string.Join("\n", new[] { "# Sailor workspace build outputs" }
            .Concat(ignoredPaths)
            .Concat(["/.vs/", "/.idea/", string.Empty]));
    }

    static async Task WriteNewFileAsync(
        string path,
        string content,
        ICollection<string> createdFiles,
        CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 4096,
            options: FileOptions.Asynchronous);
        createdFiles.Add(path);
        await using var writer = new StreamWriter(stream, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        await writer.WriteAsync(content.AsMemory(), cancellationToken);
    }

    static void CreateDirectoryTracked(string workspaceRoot, string path, ICollection<string> createdDirectories)
    {
        var root = NormalizePath(workspaceRoot);
        var current = NormalizePath(path);
        var missingDirectories = new Stack<string>();

        while (!Directory.Exists(current))
        {
            EnsureInsideWorkspace(root, current);
            missingDirectories.Push(current);
            if (string.Equals(root, current, PathComparison))
                break;

            current = Directory.GetParent(current)?.FullName
                ?? throw new InvalidOperationException($"Generated directory has no parent: {path}");
        }

        while (missingDirectories.TryPop(out var directory))
        {
            Directory.CreateDirectory(directory);
            createdDirectories.Add(directory);
        }
    }

    internal static void Rollback(WorkspaceProjectGenerationResult result)
    {
        foreach (var path in result.CreatedFiles.Reverse())
        {
            if (File.Exists(path))
                File.Delete(path);
        }

        foreach (var directory in result.CreatedDirectories.Reverse())
        {
            if (Directory.Exists(directory) && !Directory.EnumerateFileSystemEntries(directory).Any())
                Directory.Delete(directory);
        }
    }

    static void EnsureInsideWorkspace(string workspaceRoot, string path)
    {
        if (!WorkspacePathPolicy.IsInsideRoot(workspaceRoot, path))
            throw new InvalidOperationException($"Generated path resolves outside the workspace root: {path}");
    }

    static string NormalizePath(string path) => WorkspacePathPolicy.NormalizePhysicalPath(path);

    static StringComparison PathComparison => WorkspacePathPolicy.PathComparison;
}
