using System.Text;

namespace SailorEditor.Workspace;

public sealed class WorkspaceProjectGenerator
{
    public async Task GenerateAsync(WorkspaceSession session, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(session);

        var componentsDirectory = Path.Combine(session.SourceDirectory, "Components");
        var generatedFiles = new Dictionary<string, string>
        {
            [Path.Combine(session.WorkspaceRoot, ".gitignore")] = BuildGitIgnore(session.Manifest),
            [Path.Combine(session.GeneratedProjectDirectory, "CMakeLists.txt")] = BuildCMakeProject(session),
            [Path.Combine(componentsDirectory, "SampleComponent.h")] = BuildSampleComponentHeader(session.Manifest.LogicModuleName),
            [Path.Combine(componentsDirectory, "SampleComponent.cpp")] = BuildSampleComponentSource(session.Manifest.LogicModuleName)
        };

        foreach (var path in generatedFiles.Keys)
        {
            EnsureInsideWorkspace(session.WorkspaceRoot, path);
            if (File.Exists(path) || Directory.Exists(path))
                throw new InvalidOperationException($"Workspace project file already exists and will not be overwritten: {path}");
        }

        foreach (var directory in GetProjectDirectories(session, componentsDirectory))
        {
            EnsureInsideWorkspace(session.WorkspaceRoot, directory);
            Directory.CreateDirectory(directory);
        }

        foreach (var (path, content) in generatedFiles)
            await WriteNewFileAsync(path, content, cancellationToken);
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
            string.Empty,
            $"set_target_properties({moduleName} PROPERTIES",
            $"  OUTPUT_NAME \"{moduleName}\"",
            "  WINDOWS_EXPORT_ALL_SYMBOLS ON",
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
            "\t\tSAILOR_REFLECTABLE(SampleComponent)",
            string.Empty,
            "\tpublic:",
            "\t\tvoid BeginPlay() override;",
            "\t};",
            "}",
            string.Empty,
            "REFL_AUTO(",
            $"\ttype({moduleName}::SampleComponent, bases<Sailor::Component>)",
            ")",
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

    static async Task WriteNewFileAsync(string path, string content, CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 4096,
            options: FileOptions.Asynchronous);
        await using var writer = new StreamWriter(stream, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        await writer.WriteAsync(content.AsMemory(), cancellationToken);
    }

    static void EnsureInsideWorkspace(string workspaceRoot, string path)
    {
        var root = Path.GetFullPath(workspaceRoot).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var fullPath = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

        if (string.Equals(root, fullPath, PathComparison))
            return;

        if (!fullPath.StartsWith(root + Path.DirectorySeparatorChar, PathComparison))
            throw new InvalidOperationException($"Generated path resolves outside the workspace root: {path}");
    }

    static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}
