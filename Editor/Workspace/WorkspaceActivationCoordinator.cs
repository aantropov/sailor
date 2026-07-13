#nullable enable

namespace SailorEditor.Workspace;

public sealed class WorkspaceActivationCoordinator
{
    readonly IWorkspaceActivationOperations _operations;
    readonly object _queueLock = new();
    Task _queueTail = Task.CompletedTask;
    WorkspaceActivationState _state = new(WorkspaceActivationPhase.Idle, 0);
    WorkspaceActivationCandidate? _currentCandidate;
    string? _reportedRuntimeFailure;
    long _reportedRuntimeFailureGeneration = -1;
    long _generation;

    public WorkspaceActivationCoordinator(IWorkspaceActivationOperations operations)
    {
        _operations = operations ?? throw new ArgumentNullException(nameof(operations));
    }

    public WorkspaceActivationState State => Volatile.Read(ref _state);

    public event EventHandler<WorkspaceActivationState>? StateChanged;

    public bool ReportRuntimeFailure(string error)
    {
        if (string.IsNullOrWhiteSpace(error))
            throw new ArgumentException("A runtime failure message is required.", nameof(error));

        WorkspaceActivationState? repair = null;
        lock (_queueLock)
        {
            var current = State;
            if (_currentCandidate is null)
                return false;

            if (current.Phase is WorkspaceActivationPhase.Stopping or
                WorkspaceActivationPhase.Clearing or
                WorkspaceActivationPhase.Committing)
            {
                return false;
            }

            _reportedRuntimeFailure = error;
            _reportedRuntimeFailureGeneration = current.Generation;
            if (current.Phase is WorkspaceActivationPhase.Ready or WorkspaceActivationPhase.Repair)
            {
                repair = current with
                {
                    Phase = WorkspaceActivationPhase.Repair,
                    Candidate = _currentCandidate,
                    Error = error
                };
            }
        }

        if (repair is not null)
            PublishState(repair);
        return true;
    }

    public Task<WorkspaceActivationResult> ActivateAsync(
        WorkspaceActivationRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentNullException.ThrowIfNull(request.PreflightAsync);

        Task predecessor;
        var turnCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_queueLock)
        {
            predecessor = _queueTail;
            _queueTail = turnCompleted.Task;
        }

        return RunQueuedAsync(predecessor, turnCompleted, request, cancellationToken);
    }

    public Task RunSerializedAsync(
        Func<CancellationToken, Task> operation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(operation);

        Task predecessor;
        var turnCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_queueLock)
        {
            predecessor = _queueTail;
            _queueTail = turnCompleted.Task;
        }

        return RunSerializedCoreAsync(predecessor, turnCompleted, operation, cancellationToken);
    }

    static async Task RunSerializedCoreAsync(
        Task predecessor,
        TaskCompletionSource turnCompleted,
        Func<CancellationToken, Task> operation,
        CancellationToken cancellationToken)
    {
        try
        {
            await predecessor.ConfigureAwait(false);
            cancellationToken.ThrowIfCancellationRequested();
            await operation(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            turnCompleted.TrySetResult();
        }
    }

    async Task<WorkspaceActivationResult> RunQueuedAsync(
        Task predecessor,
        TaskCompletionSource turnCompleted,
        WorkspaceActivationRequest request,
        CancellationToken cancellationToken)
    {
        try
        {
            await predecessor.ConfigureAwait(false);
            return await ActivateCoreAsync(request, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            turnCompleted.TrySetResult();
        }
    }

    async Task<WorkspaceActivationResult> ActivateCoreAsync(
        WorkspaceActivationRequest request,
        CancellationToken cancellationToken)
    {
        var stableState = State;
        PublishState(new WorkspaceActivationState(
            WorkspaceActivationPhase.Preflighting,
            stableState.Generation,
            stableState.Candidate));

        WorkspaceActivationCandidate? candidate = null;
        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            candidate = await request.PreflightAsync(cancellationToken).ConfigureAwait(false)
                ?? throw new InvalidOperationException("Workspace activation preflight did not return a candidate.");
            cancellationToken.ThrowIfCancellationRequested();
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            candidate?.Preparation.Discard();
            var restored = RestoreStableState(stableState);
            return new WorkspaceActivationResult(
                restored,
                Error: restored.RequiresRepair
                    ? restored.Error
                    : $"Workspace activation '{request.Name}' was cancelled during preflight.",
                WasCancelled: true);
        }
        catch (Exception ex)
        {
            var restored = RestoreStableState(stableState);
            return new WorkspaceActivationResult(
                restored,
                Error: restored.RequiresRepair ? restored.Error : ex.Message);
        }

        var activationCandidate = candidate
            ?? throw new InvalidOperationException("Workspace activation candidate was lost after preflight.");

        // Crossing into Stopping is the irreversible boundary. From here through
        // commit, caller cancellation is intentionally ignored so teardown cannot
        // leave workspace-owned runtime objects partially alive.
        var generation = Interlocked.Increment(ref _generation);
        lock (_queueLock)
        {
            _reportedRuntimeFailure = null;
            _reportedRuntimeFailureGeneration = -1;
        }
        var failures = new List<Exception>();

        PublishState(new WorkspaceActivationState(
            WorkspaceActivationPhase.Stopping,
            generation,
            activationCandidate));
        await RunPostBoundaryStepAsync(
            () => _operations.StopAsync(CancellationToken.None),
            failures).ConfigureAwait(false);

        PublishState(new WorkspaceActivationState(
            WorkspaceActivationPhase.Clearing,
            generation,
            activationCandidate));
        await RunPostBoundaryStepAsync(
            () => _operations.ClearAsync(generation, CancellationToken.None),
            failures).ConfigureAwait(false);

        PublishState(new WorkspaceActivationState(
            WorkspaceActivationPhase.Committing,
            generation,
            activationCandidate));
        _currentCandidate = activationCandidate;
        await RunPostBoundaryStepAsync(
            () => _operations.CommitAsync(activationCandidate, CancellationToken.None),
            failures).ConfigureAwait(false);

        if (failures.Count == 0)
        {
            PublishState(new WorkspaceActivationState(
                WorkspaceActivationPhase.Starting,
                generation,
                activationCandidate));
            await RunPostBoundaryStepAsync(
                () => _operations.StartAsync(activationCandidate.LaunchContext, generation, CancellationToken.None),
                failures).ConfigureAwait(false);
        }

        lock (_queueLock)
        {
            if (_reportedRuntimeFailureGeneration == generation &&
                !string.IsNullOrWhiteSpace(_reportedRuntimeFailure))
            {
                failures.Add(new InvalidOperationException(_reportedRuntimeFailure));
            }
        }

        if (failures.Count == 0)
        {
            var ready = new WorkspaceActivationState(
                WorkspaceActivationPhase.Ready,
                generation,
                activationCandidate);
            PublishState(ready);
            return new WorkspaceActivationResult(ready, activationCandidate);
        }

        var error = FormatFailures(failures);
        var repair = new WorkspaceActivationState(
            WorkspaceActivationPhase.Repair,
            generation,
            activationCandidate,
            error);
        PublishState(repair);
        return new WorkspaceActivationResult(repair, activationCandidate, error);
    }

    WorkspaceActivationState RestoreStableState(WorkspaceActivationState previous)
    {
        lock (_queueLock)
        {
            if (_reportedRuntimeFailureGeneration == previous.Generation &&
                !string.IsNullOrWhiteSpace(_reportedRuntimeFailure))
            {
                var repair = previous with
                {
                    Phase = WorkspaceActivationPhase.Repair,
                    Candidate = _currentCandidate,
                    Error = _reportedRuntimeFailure
                };
                PublishState(repair);
                return repair;
            }
        }

        var phase = _currentCandidate is null
            ? WorkspaceActivationPhase.Idle
            : previous.Phase == WorkspaceActivationPhase.Repair
                ? WorkspaceActivationPhase.Repair
                : WorkspaceActivationPhase.Ready;
        var restored = previous with { Phase = phase };
        PublishState(restored);
        return restored;
    }

    static async Task RunPostBoundaryStepAsync(Func<Task> step, ICollection<Exception> failures)
    {
        try
        {
            await step().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            failures.Add(ex);
        }
    }

    static string FormatFailures(IReadOnlyCollection<Exception> failures)
    {
        if (failures.Count == 1)
            return failures.First().Message;

        return string.Join(
            Environment.NewLine,
            failures.Select((failure, index) => $"{index + 1}. {failure.Message}"));
    }

    void PublishState(WorkspaceActivationState state)
    {
        Volatile.Write(ref _state, state);
        var handlers = StateChanged;
        if (handlers is null)
            return;

        foreach (EventHandler<WorkspaceActivationState> handler in handlers.GetInvocationList())
        {
            try
            {
                handler(this, state);
            }
            catch
            {
                // State observers cannot interrupt the teardown transaction.
            }
        }
    }
}
