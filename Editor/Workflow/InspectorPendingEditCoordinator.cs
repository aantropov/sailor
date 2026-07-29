#nullable enable

namespace SailorEditor.Workflow;

public readonly record struct InspectorRefreshCommitOutcome(
    bool CanRefresh,
    bool ShouldRetry)
{
    public static InspectorRefreshCommitOutcome Ready { get; } =
        new(CanRefresh: true, ShouldRetry: false);

    public static InspectorRefreshCommitOutcome Blocked { get; } =
        new(CanRefresh: false, ShouldRetry: false);

    public static InspectorRefreshCommitOutcome FromCompletedCommit(
        bool commitSucceeded,
        bool hasPendingChanges)
    {
        if (!hasPendingChanges)
        {
            return Ready;
        }

        return commitSucceeded
            ? new InspectorRefreshCommitOutcome(
                CanRefresh: false,
                ShouldRetry: true)
            : Blocked;
    }
}

public static class InspectorRefreshCommitGate
{
    public static async Task<bool> TryApplyAsync(
        Func<CancellationToken, Task<InspectorRefreshCommitOutcome>>
            commitPendingChanges,
        Action applyRefresh,
        Func<bool>? shouldDiscardCommit = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(commitPendingChanges);
        ArgumentNullException.ThrowIfNull(applyRefresh);

        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (shouldDiscardCommit?.Invoke() == true)
            {
                applyRefresh();
                return true;
            }

            InspectorRefreshCommitOutcome outcome;
            try
            {
                outcome = await commitPendingChanges(cancellationToken);
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch when (shouldDiscardCommit?.Invoke() == true)
            {
                applyRefresh();
                return true;
            }

            if (shouldDiscardCommit?.Invoke() == true)
            {
                applyRefresh();
                return true;
            }

            if (outcome.CanRefresh)
            {
                applyRefresh();
                return true;
            }

            if (!outcome.ShouldRetry)
            {
                return false;
            }

            await Task.Yield();
        }
    }
}

public sealed class InspectorPendingEditCoordinator
{
    public event Func<CancellationToken, Task<bool>>?
        CommitPendingChangesRequested;

    public async Task<bool> CommitPendingChangesAsync(
        CancellationToken cancellationToken = default)
    {
        if (CommitPendingChangesRequested is not { } requested)
        {
            return true;
        }

        foreach (Func<CancellationToken, Task<bool>> handler in
            requested.GetInvocationList())
        {
            try
            {
                if (!await handler(cancellationToken))
                {
                    return false;
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector pending edit commit failed: {ex}");
                return false;
            }
        }

        return true;
    }
}
