using SailorEditor.Commands;
using SailorEditor.History;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceStateResetTests
{
    [Fact]
    public async Task ResetForWorkspaceChange_ClearsHistoryAndAdvancesEpoch()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubActionContextProvider(), history);

        var result = await dispatcher.DispatchAsync(new ImmediateCommand("Create object"), new ActionContext());
        var previousEpoch = dispatcher.WorkspaceEpoch;

        Assert.True(result.Succeeded);
        Assert.Equal(1, dispatcher.UndoCount);

        dispatcher.ResetForWorkspaceChange();

        Assert.Equal(previousEpoch + 1, dispatcher.WorkspaceEpoch);
        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(0, dispatcher.RedoCount);
    }

    [Fact]
    public async Task BeginWorkspaceChangeAsync_DrainsNonCooperativeDispatchBeforeReset()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubActionContextProvider(), history);
        var resetStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var command = new DelayedCommand("Delayed edit", () => resetStarted.Task.IsCompleted);

        Assert.True((await dispatcher.DispatchAsync(new ImmediateCommand("Existing edit"), new ActionContext())).Succeeded);
        var dispatch = dispatcher.DispatchAsync(command, new ActionContext());
        await command.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        dispatcher.BeginWorkspaceChange();
        var transition = DrainAndResetAsync(dispatcher, resetStarted);

        Assert.False(transition.IsCompleted);
        var blocked = await dispatcher.DispatchAsync(new ImmediateCommand("Blocked edit"), new ActionContext());
        Assert.False(blocked.Succeeded);
        Assert.Contains("activation is in progress", blocked.Message, StringComparison.OrdinalIgnoreCase);
        var earlyReset = Assert.Throws<InvalidOperationException>(dispatcher.ResetForWorkspaceChange);
        Assert.Contains("active command executions", earlyReset.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Equal(1, dispatcher.UndoCount);

        command.Release.TrySetResult();
        var result = await dispatch.WaitAsync(TimeSpan.FromSeconds(5));
        await transition.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(result.Succeeded);
        Assert.Contains("workspace changed", result.Message, StringComparison.OrdinalIgnoreCase);
        Assert.True(command.SideEffectApplied);
        Assert.False(command.SideEffectAfterReset);
        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(0, dispatcher.RedoCount);
    }

    [Fact]
    public async Task BeginWorkspaceChangeAsync_DrainsActiveNonCooperativeUndoBeforeReset()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubActionContextProvider(), history);
        var resetStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var command = new DelayedHistoryCommand(
            "Delayed undo",
            () => resetStarted.Task.IsCompleted,
            delayUndo: true);

        Assert.True((await dispatcher.DispatchAsync(command, new ActionContext())).Succeeded);
        var undo = dispatcher.UndoAsync();
        await command.UndoStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var transition = DrainAndResetAsync(dispatcher, resetStarted);

        Assert.False(transition.IsCompleted);
        command.ReleaseUndo.TrySetResult();
        Assert.False(await undo.WaitAsync(TimeSpan.FromSeconds(5)));
        await transition.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(command.IsApplied);
        Assert.False(command.SideEffectAfterReset);
        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(0, dispatcher.RedoCount);
    }

    [Fact]
    public async Task BeginWorkspaceChangeAsync_DrainsActiveNonCooperativeRedoBeforeReset()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubActionContextProvider(), history);
        var resetStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var command = new DelayedHistoryCommand(
            "Delayed redo",
            () => resetStarted.Task.IsCompleted,
            delayRedo: true);

        Assert.True((await dispatcher.DispatchAsync(command, new ActionContext())).Succeeded);
        Assert.True(await dispatcher.UndoAsync());
        var redo = dispatcher.RedoAsync();
        await command.RedoStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var transition = DrainAndResetAsync(dispatcher, resetStarted);

        Assert.False(transition.IsCompleted);
        command.ReleaseRedo.TrySetResult();
        Assert.False(await redo.WaitAsync(TimeSpan.FromSeconds(5)));
        await transition.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.True(command.IsApplied);
        Assert.False(command.SideEffectAfterReset);
        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(0, dispatcher.RedoCount);
    }

    [Fact]
    public async Task BeginWorkspaceChangeAsync_DrainsActiveTransactionRollbackBeforeReset()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubActionContextProvider(), history);
        var resetStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var command = new DelayedHistoryCommand(
            "Delayed transaction rollback",
            () => resetStarted.Task.IsCompleted,
            delayUndo: true);
        var transaction = await dispatcher.BeginAsync("Transaction", new ActionContext());

        Assert.True((await dispatcher.DispatchAsync(command, new ActionContext())).Succeeded);
        var rollback = transaction.DisposeAsync().AsTask();
        await command.UndoStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var transition = DrainAndResetAsync(dispatcher, resetStarted);

        Assert.False(transition.IsCompleted);
        command.ReleaseUndo.TrySetResult();
        await rollback.WaitAsync(TimeSpan.FromSeconds(5));
        await transition.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(command.IsApplied);
        Assert.False(command.SideEffectAfterReset);
        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(0, dispatcher.RedoCount);
    }

    [Fact]
    public async Task ResetForWorkspaceChange_DiscardsPriorEpochTransaction()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubActionContextProvider(), history);
        var transaction = await dispatcher.BeginAsync("Old workspace edits", new ActionContext());

        var result = await dispatcher.DispatchAsync(new ImmediateCommand("Rename object"), new ActionContext());
        Assert.True(result.Succeeded);
        Assert.Equal(0, dispatcher.UndoCount);

        dispatcher.ResetForWorkspaceChange();
        await transaction.CompleteAsync();
        await transaction.DisposeAsync();

        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(0, dispatcher.RedoCount);
    }

    [Fact]
    public async Task WorkspaceChange_GatesCommandsUntilActivationCompletes()
    {
        var dispatcher = new EditorCommandDispatcher(
            new StubActionContextProvider(),
            new InMemoryUndoRedoHistory());

        dispatcher.BeginWorkspaceChange();
        dispatcher.ResetForWorkspaceChange();
        var blocked = await dispatcher.DispatchAsync(
            new ImmediateCommand("Blocked edit"),
            new ActionContext(WorkspaceEpoch: dispatcher.WorkspaceEpoch));

        Assert.False(blocked.Succeeded);
        Assert.Contains("activation is in progress", blocked.Message, StringComparison.OrdinalIgnoreCase);

        dispatcher.CompleteWorkspaceChange();
        var accepted = await dispatcher.DispatchAsync(
            new ImmediateCommand("New workspace edit"),
            new ActionContext(WorkspaceEpoch: dispatcher.WorkspaceEpoch));

        Assert.True(accepted.Succeeded);
        Assert.Equal(1, dispatcher.UndoCount);
    }

    [Fact]
    public async Task CompletedActivation_RejectsContextCapturedFromPriorWorkspace()
    {
        var dispatcher = new EditorCommandDispatcher(
            new StubActionContextProvider(),
            new InMemoryUndoRedoHistory());
        var oldContext = new ActionContext(WorkspaceEpoch: dispatcher.WorkspaceEpoch);

        dispatcher.BeginWorkspaceChange();
        dispatcher.ResetForWorkspaceChange();
        dispatcher.CompleteWorkspaceChange();
        var result = await dispatcher.DispatchAsync(new ImmediateCommand("Stale edit"), oldContext);

        Assert.False(result.Succeeded);
        Assert.Contains("workspace changed", result.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Equal(0, dispatcher.UndoCount);
    }

    sealed class StubActionContextProvider : IActionContextProvider
    {
        public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null) =>
            new(Origin: origin, Metadata: metadata);
    }

    sealed class ImmediateCommand(string description) : IUndoableEditorCommand
    {
        public string Name => description;
        public string Description => description;
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) =>
            Task.FromResult(CommandResult.Success());
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(CommandResult.Success());
    }

    static async Task DrainAndResetAsync(
        EditorCommandDispatcher dispatcher,
        TaskCompletionSource resetStarted)
    {
        await dispatcher.BeginWorkspaceChangeAsync();
        resetStarted.TrySetResult();
        dispatcher.ResetForWorkspaceChange();
    }

    sealed class DelayedCommand(string description, Func<bool> hasResetStarted) : IUndoableEditorCommand
    {
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public string Name => description;
        public string Description => description;
        public bool SideEffectApplied { get; private set; }
        public bool SideEffectAfterReset { get; private set; }
        public bool CanExecute(ActionContext context) => true;

        public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            Started.TrySetResult();
            await Release.Task;
            SideEffectAfterReset = hasResetStarted();
            SideEffectApplied = true;
            return CommandResult.Success();
        }

        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(CommandResult.Success());
    }

    sealed class DelayedHistoryCommand(
        string description,
        Func<bool> hasResetStarted,
        bool delayUndo = false,
        bool delayRedo = false) : IUndoableEditorCommand
    {
        int executeCount;

        public TaskCompletionSource UndoStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseUndo { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource RedoStarted { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseRedo { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public string Name => description;
        public string Description => description;
        public bool IsApplied { get; private set; }
        public bool SideEffectAfterReset { get; private set; }
        public bool CanExecute(ActionContext context) => !IsApplied;

        public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            executeCount++;
            if (executeCount > 1 && delayRedo)
            {
                RedoStarted.TrySetResult();
                await ReleaseRedo.Task;
                SideEffectAfterReset = hasResetStarted();
            }

            IsApplied = true;
            return CommandResult.Success();
        }

        public async ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            if (delayUndo)
            {
                UndoStarted.TrySetResult();
                await ReleaseUndo.Task;
                SideEffectAfterReset = hasResetStarted();
            }

            IsApplied = false;
            return CommandResult.Success();
        }
    }
}
