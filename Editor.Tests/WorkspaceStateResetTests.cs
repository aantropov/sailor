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
    public async Task ResetForWorkspaceChange_RejectsDelayedPriorEpochCommand()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubActionContextProvider(), history);
        var command = new DelayedCommand("Delayed edit");

        var dispatch = dispatcher.DispatchAsync(command, new ActionContext());
        await command.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        dispatcher.ResetForWorkspaceChange();
        command.Release.TrySetResult();
        var result = await dispatch.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(result.Succeeded);
        Assert.Contains("workspace changed", result.Message, StringComparison.OrdinalIgnoreCase);
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
        public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    sealed class DelayedCommand(string description) : IUndoableEditorCommand
    {
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public string Name => description;
        public string Description => description;
        public bool CanExecute(ActionContext context) => true;

        public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            Started.TrySetResult();
            await Release.Task;
            return CommandResult.Success();
        }

        public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }
}
