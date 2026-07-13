using System.Diagnostics;
using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

internal static class WorkspaceCMakeIntegrationHarness
{
    public static bool ShouldRun
        => string.Equals(
            Environment.GetEnvironmentVariable("SAILOR_RUN_CMAKE_INTEGRATION"),
            "1",
            StringComparison.Ordinal);

    public static void EnsureWindows()
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("Workspace CMake integration tests require Windows.");
    }

    public static string GetRequiredEnvironmentVariable(string name)
    {
        var value = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"{name} is required when SAILOR_RUN_CMAKE_INTEGRATION=1.");

        return value;
    }

    public static async Task<string> ConfigureBuildAndAssertAsync(WorkspaceSession session)
    {
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
            "--target", session.Manifest.LogicModuleName,
            "--parallel"
        ]);

        var expectedDll = GetReleaseModulePath(session);
        var expectedImportLibrary = Path.ChangeExtension(expectedDll, ".lib");
        Assert.True(File.Exists(expectedDll), $"Expected generated logic module was not found: {expectedDll}");
        Assert.True(
            File.Exists(expectedImportLibrary),
            $"Expected generated logic import library was not found: {expectedImportLibrary}");
        return expectedDll;
    }

    public static string GetReleaseModulePath(WorkspaceSession session)
        => Path.Combine(
            session.LogicOutputDirectory,
            "Release",
            $"{session.Manifest.LogicModuleName}.dll");

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
}
