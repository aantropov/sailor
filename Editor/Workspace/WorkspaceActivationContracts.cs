#nullable enable

using SailorEditor.Services;

namespace SailorEditor.Workspace;

public enum WorkspaceActivationPhase
{
    Idle,
    Preflighting,
    Stopping,
    Clearing,
    Committing,
    Starting,
    Ready,
    Repair
}

public sealed record WorkspaceActivationCandidate(
    WorkspaceLifecyclePreparation Preparation,
    EngineLaunchContext LaunchContext)
{
    public WorkspaceSession Session => Preparation.Session;
}

public sealed record WorkspaceActivationRequest(
    string Name,
    Func<CancellationToken, Task<WorkspaceActivationCandidate>> PreflightAsync);

public sealed record WorkspaceActivationState(
    WorkspaceActivationPhase Phase,
    long Generation,
    WorkspaceActivationCandidate? Candidate = null,
    string? Error = null)
{
    public bool IsReady => Phase == WorkspaceActivationPhase.Ready;
    public bool RequiresRepair => Phase == WorkspaceActivationPhase.Repair;
}

public sealed record WorkspaceActivationResult(
    WorkspaceActivationState State,
    WorkspaceActivationCandidate? Candidate = null,
    string? Error = null,
    bool WasCancelled = false)
{
    public bool Succeeded => State.IsReady && Candidate is not null && string.IsNullOrEmpty(Error);
    public bool RequiresRepair => State.RequiresRepair;
}

public interface IWorkspaceActivationOperations
{
    Task StopAsync(CancellationToken cancellationToken);
    Task ClearAsync(long generation, CancellationToken cancellationToken);
    Task CommitAsync(WorkspaceActivationCandidate candidate, CancellationToken cancellationToken);
    Task StartAsync(EngineLaunchContext launchContext, long generation, CancellationToken cancellationToken);
}
