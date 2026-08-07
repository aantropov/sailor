#nullable enable

using System.Diagnostics;
using System.Text;

namespace SailorEditor.Workspace;

public sealed record WorkspaceProcessInvocation(
    string FileName,
    IReadOnlyList<string> Arguments,
    string WorkingDirectory);

public sealed record WorkspaceProcessResult(
    int ExitCode,
    string Output,
    TimeSpan Duration)
{
    public bool Succeeded => ExitCode == 0;
}

public sealed record WorkspaceBuildPlan(
    string Configuration,
    IReadOnlyList<WorkspaceProcessInvocation> Invocations)
{
    public static WorkspaceBuildPlan Create(
        WorkspaceSession session,
        string configuration,
        bool configure,
        string cmakeExecutable = "cmake")
    {
        ArgumentNullException.ThrowIfNull(session);
        var normalizedConfiguration = NormalizeConfiguration(configuration);
        var invocations = new List<WorkspaceProcessInvocation>(configure ? 2 : 1);
        if (configure)
        {
            var configureArguments = new List<string>
            {
                "-S",
                session.GeneratedProjectDirectory,
                "-B",
                session.BuildDirectory,
                "-DCMAKE_BUILD_TYPE=" + normalizedConfiguration,
            };
            AddVcpkgArguments(session, configureArguments);
            invocations.Add(new WorkspaceProcessInvocation(
                cmakeExecutable,
                configureArguments,
                session.WorkspaceRoot));
        }

        invocations.Add(new WorkspaceProcessInvocation(
            cmakeExecutable,
            [
                "--build",
                session.BuildDirectory,
                "--config",
                normalizedConfiguration,
                "--target",
                session.Manifest.LogicModuleName,
                "--parallel",
                "4",
            ],
            session.WorkspaceRoot));
        return new WorkspaceBuildPlan(normalizedConfiguration, invocations);
    }

    static void AddVcpkgArguments(
        WorkspaceSession session,
        ICollection<string> arguments)
    {
        var configuredEnginePath = session.Manifest.EnginePath
            .Replace('/', Path.DirectorySeparatorChar)
            .Replace('\\', Path.DirectorySeparatorChar);
        var engineDirectory = Path.IsPathRooted(configuredEnginePath)
            ? Path.GetFullPath(configuredEnginePath)
            : Path.GetFullPath(configuredEnginePath, session.WorkspaceRoot);
        var toolchainPath = Path.Combine(
            engineDirectory,
            "External",
            "vcpkg",
            "scripts",
            "buildsystems",
            "vcpkg.cmake");
        if (!File.Exists(toolchainPath))
            return;

        arguments.Add("-DCMAKE_TOOLCHAIN_FILE=" + toolchainPath);
        var triplet = ResolveVcpkgTriplet();
        if (triplet is null)
            return;

        arguments.Add("-DVCPKG_TARGET_TRIPLET=" + triplet);
        var installedDirectory = Path.Combine(
            engineDirectory,
            "External",
            "vcpkg",
            "installed",
            triplet);
        if (!Directory.Exists(installedDirectory))
            return;

        // CMAKE_TOOLCHAIN_FILE is ignored by an already configured CMake cache.
        // Prefix/module paths keep an existing workspace build cache usable too.
        arguments.Add("-DCMAKE_PREFIX_PATH=" + installedDirectory);
        var stbModules = Path.Combine(installedDirectory, "share", "stb");
        if (Directory.Exists(stbModules))
            arguments.Add("-DCMAKE_MODULE_PATH=" + stbModules);
    }

    static string? ResolveVcpkgTriplet()
    {
        if (OperatingSystem.IsMacOS() || OperatingSystem.IsMacCatalyst())
            return "arm64-osx";
        if (OperatingSystem.IsWindows())
            return "x64-windows";
        if (OperatingSystem.IsLinux())
            return "x64-linux";
        return null;
    }

    static string NormalizeConfiguration(string? configuration)
    {
        var value = configuration?.Trim();
        if (string.IsNullOrWhiteSpace(value))
            return "Release";
        if (value is not ("Debug" or "Release" or "RelWithDebInfo" or "MinSizeRel"))
        {
            throw new ArgumentOutOfRangeException(
                nameof(configuration),
                "Configuration must be Debug, Release, RelWithDebInfo or MinSizeRel.");
        }

        return value;
    }
}

public sealed record WorkspaceBuildResult(
    bool Succeeded,
    string Configuration,
    int? ExitCode,
    TimeSpan Duration,
    IReadOnlyList<string> Commands,
    string Output,
    string? Error = null);

public interface IWorkspaceProcessRunner
{
    Task<WorkspaceProcessResult> RunAsync(
        WorkspaceProcessInvocation invocation,
        CancellationToken cancellationToken = default);
}

public sealed class WorkspaceProcessRunner : IWorkspaceProcessRunner
{
    const int MaximumCapturedCharacters = 1_000_000;

    public async Task<WorkspaceProcessResult> RunAsync(
        WorkspaceProcessInvocation invocation,
        CancellationToken cancellationToken = default)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = invocation.FileName,
            WorkingDirectory = invocation.WorkingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (var argument in invocation.Arguments)
            startInfo.ArgumentList.Add(argument);

        using var process = new Process { StartInfo = startInfo };
        var stopwatch = Stopwatch.StartNew();
        try
        {
            if (!process.Start())
                throw new InvalidOperationException($"Unable to start '{invocation.FileName}'.");
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            return new WorkspaceProcessResult(
                -1,
                exception.Message,
                stopwatch.Elapsed);
        }

        using var cancellationRegistration = cancellationToken.Register(() =>
        {
            try
            {
                if (!process.HasExited)
                    process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
            }
        });

        var standardOutput = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var standardError = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);
        var output = await standardOutput;
        var error = await standardError;
        stopwatch.Stop();

        var combined = new StringBuilder();
        if (!string.IsNullOrWhiteSpace(output))
            combined.AppendLine(output.TrimEnd());
        if (!string.IsNullOrWhiteSpace(error))
            combined.AppendLine(error.TrimEnd());
        if (combined.Length > MaximumCapturedCharacters)
        {
            combined.Remove(0, combined.Length - MaximumCapturedCharacters);
            combined.Insert(0, "[Output truncated to the final 1,000,000 characters]\n");
        }

        return new WorkspaceProcessResult(
            process.ExitCode,
            combined.ToString(),
            stopwatch.Elapsed);
    }
}

internal sealed class WorkspaceBuildService
{
    readonly WorkspaceLifecycleService _workspaceLifecycle;
    readonly IWorkspaceProcessRunner _processRunner;
    readonly SemaphoreSlim _buildGate = new(1, 1);

    public WorkspaceBuildService(
        WorkspaceLifecycleService workspaceLifecycle,
        IWorkspaceProcessRunner processRunner)
    {
        _workspaceLifecycle = workspaceLifecycle;
        _processRunner = processRunner;
    }

    public async Task<WorkspaceBuildResult> BuildAsync(
        string configuration,
        bool configure,
        CancellationToken cancellationToken = default)
    {
        var session = _workspaceLifecycle.Current;
        if (session is null)
        {
            return new WorkspaceBuildResult(
                false,
                configuration,
                null,
                TimeSpan.Zero,
                Array.Empty<string>(),
                string.Empty,
                "An active workspace is required for a workspace build.");
        }
        if (session.GeneratedProjectState.RequiresAttention)
        {
            return new WorkspaceBuildResult(
                false,
                configuration,
                null,
                TimeSpan.Zero,
                Array.Empty<string>(),
                string.Empty,
                session.GeneratedProjectState.Guidance);
        }

        WorkspaceBuildPlan plan;
        try
        {
            plan = WorkspaceBuildPlan.Create(session, configuration, configure);
        }
        catch (ArgumentException exception)
        {
            return new WorkspaceBuildResult(
                false,
                configuration,
                null,
                TimeSpan.Zero,
                Array.Empty<string>(),
                string.Empty,
                exception.Message);
        }

        await _buildGate.WaitAsync(cancellationToken);
        try
        {
            Directory.CreateDirectory(session.BuildDirectory);
            var output = new StringBuilder();
            var commands = new List<string>(plan.Invocations.Count);
            var duration = TimeSpan.Zero;
            foreach (var invocation in plan.Invocations)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var displayCommand = FormatCommand(invocation);
                commands.Add(displayCommand);
                output.AppendLine("$ " + displayCommand);
                var result = await _processRunner.RunAsync(
                    invocation,
                    cancellationToken);
                duration += result.Duration;
                output.Append(result.Output);
                if (!result.Succeeded)
                {
                    return new WorkspaceBuildResult(
                        false,
                        plan.Configuration,
                        result.ExitCode,
                        duration,
                        commands,
                        output.ToString(),
                        $"Workspace build command failed with exit code {result.ExitCode}.");
                }
            }

            return new WorkspaceBuildResult(
                true,
                plan.Configuration,
                0,
                duration,
                commands,
                output.ToString());
        }
        finally
        {
            _buildGate.Release();
        }
    }

    static string FormatCommand(WorkspaceProcessInvocation invocation) =>
        invocation.FileName + " " + string.Join(" ", invocation.Arguments.Select(Quote));

    static string Quote(string argument) =>
        argument.Any(char.IsWhiteSpace)
            ? "\"" + argument.Replace("\"", "\\\"") + "\""
            : argument;
}
