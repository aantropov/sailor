using SailorEditor.Commands;
using SailorEditor.Content;
using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class WorkspaceActivationOperations(
    EngineService engineService,
    WorkspaceLifecycleService workspaceLifecycle,
    SelectionService selectionService,
    InspectorProjectionService inspectorProjection,
    HierarchyProjectionService hierarchyProjection,
    WorldService worldService,
    AssetsService assetsService,
    ProjectContentStore projectContentStore,
    ICommandHistoryService commandHistory) : IWorkspaceActivationOperations
{
    public async Task StopAsync(CancellationToken cancellationToken)
    {
        var failures = new List<Exception>();
        Reset(commandHistory.BeginWorkspaceChange, failures);
        Reset(selectionService.BeginWorkspaceChange, failures);

        var commandExecutionsDrained = false;
        try
        {
            // Teardown is irreversible. Once new commands are gated, old workspace
            // executions must finish even when the activation request was cancelled.
            await commandHistory.BeginWorkspaceChangeAsync(CancellationToken.None).ConfigureAwait(false);
            commandExecutionsDrained = true;
        }
        catch (Exception ex)
        {
            failures.Add(ex);
        }

        if (!commandExecutionsDrained)
            throw new AggregateException("Workspace command execution drain failed.", failures);

        var hadActiveRuntime = engineService.State is
            EngineLifecycleState.Starting or
            EngineLifecycleState.Running or
            EngineLifecycleState.Stopping;
        try
        {
            var exitCode = await engineService.StopAsync(cancellationToken).ConfigureAwait(false);
            if (hadActiveRuntime && exitCode != 0)
            {
                failures.Add(new InvalidOperationException(
                    $"The previous workspace runtime stopped with exit code {exitCode}."));
            }
        }
        catch (Exception ex)
        {
            failures.Add(ex);
        }

        if (failures.Count > 0)
            throw new AggregateException("Workspace deactivation failed.", failures);
    }

    public Task ClearAsync(long generation, CancellationToken cancellationToken)
    {
        _ = generation;
        cancellationToken.ThrowIfCancellationRequested();

        return MainThread.InvokeOnMainThreadAsync(() =>
        {
            var failures = new List<Exception>();
            // This guard must succeed before any workspace-owned runtime or
            // projection state is cleared.
            commandHistory.ResetForWorkspaceChange();
            Reset(selectionService.ResetForWorkspaceChange, failures);
            Reset(worldService.ResetForWorkspaceChange, failures);
            Reset(engineService.ResetForWorkspaceChange, failures);
            Reset(assetsService.ResetForWorkspaceChange, failures);
            Reset(() => projectContentStore.ResetForWorkspaceChange(), failures);
            Reset(hierarchyProjection.ResetForWorkspaceChange, failures);
            Reset(inspectorProjection.ResetForWorkspaceChange, failures);

            if (failures.Count > 0)
            {
                throw new AggregateException(
                    "One or more editor workspace projections could not be cleared.",
                    failures);
            }
        });
    }

    public async Task CommitAsync(
        WorkspaceActivationCandidate candidate,
        CancellationToken cancellationToken)
    {
        await workspaceLifecycle.CommitActivationAsync(candidate.Preparation, cancellationToken)
            .ConfigureAwait(false);
    }

    public async Task StartAsync(
        EngineLaunchContext launchContext,
        long generation,
        CancellationToken cancellationToken)
    {
        _ = generation;
        try
        {
            await engineService.StartAsync(launchContext, cancellationToken).ConfigureAwait(false);
            await MainThread.InvokeOnMainThreadAsync(() =>
            {
                assetsService.AddProjectRoot(launchContext.ContentDirectory);
                projectContentStore.Refresh();
            });

            if (engineService.State != EngineLifecycleState.Running)
            {
                throw new InvalidOperationException(
                    $"The candidate runtime did not remain running (state: {engineService.State}).");
            }
        }
        catch (Exception activationFailure)
        {
            try
            {
                await engineService.StopAsync(CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception teardownFailure)
            {
                throw new AggregateException(
                    "Candidate startup failed and its runtime could not be stopped cleanly.",
                    activationFailure,
                    teardownFailure);
            }

            throw;
        }

        commandHistory.CompleteWorkspaceChange();
        selectionService.CompleteWorkspaceChange();
    }

    static void Reset(Action action, ICollection<Exception> failures)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            failures.Add(ex);
        }
    }
}
