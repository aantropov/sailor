#nullable enable

using System.Diagnostics;
using System.Text;

namespace SailorEditor.Workspace;

public sealed record WorkspaceBuildPlan(
    string Configuration,
    IReadOnlyList<WorkspaceProcessInvocation> Invocations)
{
    public static WorkspaceBuildPlan CreateConfigure(
        WorkspaceSession session,
        string configuration,
        string cmakeExecutable = "cmake",
        string? engineDirectory = null,
        string? engineSdkDirectory = null)
    {
        ArgumentNullException.ThrowIfNull(session);
        var normalizedConfiguration = NormalizeConfiguration(configuration);
        return new WorkspaceBuildPlan(
            normalizedConfiguration,
            [CreateConfigureInvocation(session, normalizedConfiguration, cmakeExecutable, engineDirectory, engineSdkDirectory)]);
    }

    public static WorkspaceBuildPlan Create(
        WorkspaceSession session,
        string configuration,
        bool configure,
        string cmakeExecutable = "cmake",
        string? engineDirectory = null,
        string? engineSdkDirectory = null)
    {
        ArgumentNullException.ThrowIfNull(session);
        var normalizedConfiguration = NormalizeConfiguration(configuration);
        // A project may have been configured by another editor checkout.
        configure |= engineDirectory is not null || engineSdkDirectory is not null;
        var invocations = new List<WorkspaceProcessInvocation>(configure ? 2 : 1);
        if (configure)
        {
            invocations.Add(CreateConfigureInvocation(session, normalizedConfiguration, cmakeExecutable, engineDirectory, engineSdkDirectory));
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

    static WorkspaceProcessInvocation CreateConfigureInvocation(
        WorkspaceSession session,
        string configuration,
        string cmakeExecutable,
        string? engineDirectory,
        string? engineSdkDirectory)
    {
        var arguments = new List<string>
        {
            "-S",
            session.GeneratedProjectDirectory,
            "-B",
            session.BuildDirectory,
            "-DCMAKE_BUILD_TYPE=" + configuration,
        };
        if (engineSdkDirectory is not null)
        {
            engineSdkDirectory = Path.GetFullPath(engineSdkDirectory);
            arguments.Add("-DSAILOR_ENGINE_ROOT=" + engineSdkDirectory);
            arguments.Add("-DSAILOR_ENGINE_REFERENCE_KIND=installed");
            arguments.Add("-DSailor_DIR=" + Path.Combine(engineSdkDirectory, "lib", "cmake", "Sailor"));
        }
        else if (engineDirectory is not null)
        {
            engineDirectory = Path.GetFullPath(engineDirectory);
            arguments.Add("-DSAILOR_ENGINE_ROOT=" + engineDirectory);
            arguments.Add("-DSAILOR_ENGINE_REFERENCE_KIND=" +
                WorkspaceEngineReferenceKinds.FromEngineRoot(engineDirectory));
        }
        AddVcpkgArguments(session, arguments, engineDirectory);
        return new WorkspaceProcessInvocation(
            cmakeExecutable,
            arguments,
            session.WorkspaceRoot);
    }

    static void AddVcpkgArguments(
        WorkspaceSession session,
        ICollection<string> arguments,
        string? engineDirectory)
    {
        var configuredEnginePath = session.Manifest.EnginePath
            .Replace('/', Path.DirectorySeparatorChar)
            .Replace('\\', Path.DirectorySeparatorChar);
        engineDirectory ??= Path.IsPathRooted(configuredEnginePath)
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

internal sealed class WorkspaceBuildService
{
    readonly WorkspaceLifecycleService _workspaceLifecycle;
    readonly IWorkspaceProcessRunner _processRunner;
    readonly string? _engineDirectory;
    readonly string? _engineSdkDirectory;
    readonly SemaphoreSlim _buildGate = new(1, 1);

    public WorkspaceBuildService(
        WorkspaceLifecycleService workspaceLifecycle,
        IWorkspaceProcessRunner processRunner,
        string? engineDirectory = null,
        string? engineSdkDirectory = null)
    {
        _workspaceLifecycle = workspaceLifecycle;
        _processRunner = processRunner;
        _engineDirectory = engineDirectory;
        _engineSdkDirectory = engineSdkDirectory;
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

        return await BuildAsync(session, configuration, configure, cancellationToken);
    }

    public async Task<WorkspaceBuildResult> BuildAsync(
        WorkspaceSession session,
        string configuration,
        bool configure,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(session);
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
            plan = WorkspaceBuildPlan.Create(session, configuration, configure,
                engineDirectory: _engineDirectory, engineSdkDirectory: _engineSdkDirectory);
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

        return await RunAsync(session, plan, cancellationToken);
    }

    public Task<WorkspaceBuildResult> ConfigureAsync(
        WorkspaceSession session,
        string configuration,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(session);
        if (session.GeneratedProjectState.RequiresAttention)
        {
            return Task.FromResult(new WorkspaceBuildResult(
                false,
                configuration,
                null,
                TimeSpan.Zero,
                Array.Empty<string>(),
                string.Empty,
                session.GeneratedProjectState.Guidance));
        }

        WorkspaceBuildPlan plan;
        try
        {
            plan = WorkspaceBuildPlan.CreateConfigure(session, configuration,
                engineDirectory: _engineDirectory, engineSdkDirectory: _engineSdkDirectory);
        }
        catch (ArgumentException exception)
        {
            return Task.FromResult(new WorkspaceBuildResult(
                false,
                configuration,
                null,
                TimeSpan.Zero,
                Array.Empty<string>(),
                string.Empty,
                exception.Message));
        }

        return RunAsync(session, plan, cancellationToken);
    }

    async Task<WorkspaceBuildResult> RunAsync(
        WorkspaceSession session,
        WorkspaceBuildPlan plan,
        CancellationToken cancellationToken)
    {
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
