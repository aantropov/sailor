using SailorEditor.Commands;
using SailorEditor.History;

namespace SailorEditor.Editor.Tests;

public class HistoryContractsTests
{
    [Fact]
    public void InMemoryHistory_PushUndoRedoClear_FollowsExpectedSemantics()
    {
        var history = new InMemoryUndoRedoHistory();
        var first = new HistoryEntry("Create object", new StubUndoableCommand("Create object"));
        var second = new HistoryEntry("Delete object", new StubUndoableCommand("Delete object"));

        history.Push(first);
        history.Push(second);

        Assert.Equal(2, history.UndoCount);
        Assert.Equal("Delete object", history.PeekUndo()!.Description);

        var poppedUndo = history.PopUndo();
        history.PushRedo(poppedUndo);

        Assert.Equal(1, history.UndoCount);
        Assert.Equal(1, history.RedoCount);
        Assert.Equal("Delete object", history.PeekRedo()!.Description);

        history.Push(new HistoryEntry("Reparent object", new StubUndoableCommand("Reparent object")));

        Assert.Equal(2, history.UndoCount);
        Assert.Equal(0, history.RedoCount);

        history.Clear();

        Assert.Equal(0, history.UndoCount);
        Assert.Equal(0, history.RedoCount);
        Assert.Null(history.PeekUndo());
        Assert.Null(history.PeekRedo());
    }

    [Fact]
    public async Task Dispatcher_CoalescesRapidEditsWithoutLosingTheOriginalUndoState()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var context = new ActionContext(Origin: new CommandOrigin(CommandOriginKind.Test, "HistoryContracts"));

        await dispatcher.DispatchAsync(new StubCoalescibleCommand("object-1", "state-0", "state-1", "Edit A"), context);
        await dispatcher.DispatchAsync(new StubCoalescibleCommand("object-1", "state-1", "state-2", "Edit AB"), context);

        Assert.Equal(1, history.UndoCount);
        var merged = Assert.IsType<StubCoalescibleCommand>(history.PeekUndo()!.Command);
        Assert.Equal("state-0", merged.Before);
        Assert.Equal("state-2", merged.After);
        Assert.Equal("Edit AB", merged.Description);
    }

    [Fact]
    public async Task Dispatcher_DoesNotCoalesceEditsForDifferentTargets()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var context = new ActionContext(Origin: new CommandOrigin(CommandOriginKind.Test, "HistoryContracts"));

        await dispatcher.DispatchAsync(new StubCoalescibleCommand("object-1", "a", "b", "Edit object"), context);
        await dispatcher.DispatchAsync(new StubCoalescibleCommand("object-2", "x", "y", "Edit object"), context);

        Assert.Equal(2, history.UndoCount);
    }

    [Fact]
    public async Task Dispatcher_DoesNotCoalesceAcrossAnUndoBranch()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var context = new ActionContext(Origin: new CommandOrigin(CommandOriginKind.Test, "HistoryContracts"));

        await dispatcher.DispatchAsync(new StubCoalescibleCommand("object-1", "state-0", "state-1", "Edit object"), context);
        await dispatcher.DispatchAsync(new StubUndoableCommand("Delete object"), context);
        Assert.True(await dispatcher.UndoAsync());

        await dispatcher.DispatchAsync(new StubCoalescibleCommand("object-1", "state-1", "state-2", "Edit object"), context);

        Assert.Equal(2, history.UndoCount);
        Assert.Equal(0, history.RedoCount);
    }

    [Fact]
    public async Task Dispatcher_FailedUndoRemainsAvailableForRetry()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var command = new StubRecoverableUndoCommand();

        await dispatcher.DispatchAsync(command, new ActionContext());

        Assert.False(await dispatcher.UndoAsync());
        Assert.Equal(1, history.UndoCount);
        Assert.Equal(0, history.RedoCount);

        command.AllowUndo = true;
        Assert.True(await dispatcher.UndoAsync());
        Assert.Equal(0, history.UndoCount);
        Assert.Equal(1, history.RedoCount);
        Assert.Equal(2, command.UndoAttempts);
    }

    [Fact]
    public async Task Dispatcher_RedoPreservesRemainingRedoEntries()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);

        await dispatcher.DispatchAsync(new StubUndoableCommand("First edit"), new ActionContext());
        await dispatcher.DispatchAsync(new StubUndoableCommand("Second edit"), new ActionContext());
        Assert.True(await dispatcher.UndoAsync());
        Assert.True(await dispatcher.UndoAsync());
        Assert.Equal(2, history.RedoCount);

        Assert.True(await dispatcher.RedoAsync());
        Assert.Equal(1, history.UndoCount);
        Assert.Equal(1, history.RedoCount);

        Assert.True(await dispatcher.RedoAsync());
        Assert.Equal(2, history.UndoCount);
        Assert.Equal(0, history.RedoCount);
    }

    [Fact]
    public async Task Dispatcher_CancelledUndoRemainsAvailableForRetry()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var command = new StubCancellableCommand(cancelUndo: true, cancelRedo: false);

        await dispatcher.DispatchAsync(command, new ActionContext());

        Assert.False(await dispatcher.UndoAsync());
        Assert.Equal(1, history.UndoCount);
        Assert.Equal(0, history.RedoCount);
    }

    [Fact]
    public async Task Dispatcher_CancelledRedoRemainsAvailableForRetry()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var command = new StubCancellableCommand(cancelUndo: false, cancelRedo: true);

        await dispatcher.DispatchAsync(command, new ActionContext());
        Assert.True(await dispatcher.UndoAsync());

        Assert.False(await dispatcher.RedoAsync());
        Assert.Equal(0, history.UndoCount);
        Assert.Equal(1, history.RedoCount);
    }

    [Fact]
    public async Task Dispatcher_RejectsDispatchWhileUndoIsInProgressBeforeCommandMutation()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var blocking = new BlockingUndoCommand("Blocking edit");
        var concurrent = new StateTrackingCommand("Concurrent dispatch");

        Assert.True((await dispatcher.DispatchAsync(blocking, new ActionContext())).Succeeded);
        var activeUndo = dispatcher.UndoAsync();
        await blocking.UndoStarted.WaitAsync(TimeSpan.FromSeconds(5));

        var result = await dispatcher.DispatchAsync(concurrent, new ActionContext());
        blocking.ReleaseUndo();

        Assert.True(await activeUndo);
        Assert.False(result.Succeeded);
        Assert.Contains("undo or redo operation is in progress", result.Message);
        Assert.Equal(0, concurrent.ExecuteCount);
        Assert.False(concurrent.IsApplied);
    }

    [Fact]
    public async Task Dispatcher_RejectsSecondUndoWhileUndoIsInProgressBeforeCommandMutation()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var earlier = new StateTrackingCommand("Earlier edit");
        var blocking = new BlockingUndoCommand("Blocking edit");

        Assert.True((await dispatcher.DispatchAsync(earlier, new ActionContext())).Succeeded);
        Assert.True((await dispatcher.DispatchAsync(blocking, new ActionContext())).Succeeded);
        var activeUndo = dispatcher.UndoAsync();
        await blocking.UndoStarted.WaitAsync(TimeSpan.FromSeconds(5));

        var concurrentResult = await dispatcher.UndoAsync();
        blocking.ReleaseUndo();

        Assert.True(await activeUndo);
        Assert.False(concurrentResult);
        Assert.Equal(0, earlier.UndoCount);
        Assert.True(earlier.IsApplied);
    }

    [Fact]
    public async Task Dispatcher_RejectsRedoWhileUndoIsInProgressBeforeCommandMutation()
    {
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var blocking = new BlockingUndoCommand("Blocking edit");
        var redoCandidate = new StateTrackingCommand("Redo candidate");

        Assert.True((await dispatcher.DispatchAsync(blocking, new ActionContext())).Succeeded);
        Assert.True((await dispatcher.DispatchAsync(redoCandidate, new ActionContext())).Succeeded);
        Assert.True(await dispatcher.UndoAsync());
        Assert.False(redoCandidate.IsApplied);
        Assert.Equal(1, redoCandidate.ExecuteCount);

        var activeUndo = dispatcher.UndoAsync();
        await blocking.UndoStarted.WaitAsync(TimeSpan.FromSeconds(5));

        var concurrentResult = await dispatcher.RedoAsync();
        blocking.ReleaseUndo();

        Assert.True(await activeUndo);
        Assert.False(concurrentResult);
        Assert.Equal(1, redoCandidate.ExecuteCount);
        Assert.False(redoCandidate.IsApplied);
    }

    [Fact]
    public async Task CompoundExecute_FailedChildRollsBackPriorChildrenAndCanRetryWithoutDoubleApply()
    {
        var operations = new List<string>();
        var first = new ScriptedStateCommand("First", operations);
        var second = new ScriptedStateCommand(
            "Second",
            operations,
            executeBehaviors: [CommandBehavior.Failure, CommandBehavior.Success]);
        var compound = new CompoundEditorCommand("Compound edit", [first, second]);

        var failed = await compound.ExecuteAsync(new ActionContext());

        Assert.False(failed.Succeeded);
        Assert.False(first.IsApplied);
        Assert.False(second.IsApplied);
        Assert.Equal(["execute:First", "execute:Second", "undo:First"], operations);

        operations.Clear();
        var retried = await compound.ExecuteAsync(new ActionContext());

        Assert.True(retried.Succeeded);
        Assert.True(first.IsApplied);
        Assert.True(second.IsApplied);
        Assert.Equal(["execute:First", "execute:Second"], operations);
    }

    [Fact]
    public async Task CompoundExecute_ThrownChildRollsBackPriorChildrenAndRethrows()
    {
        var operations = new List<string>();
        var first = new ScriptedStateCommand("First", operations);
        var second = new ScriptedStateCommand(
            "Second",
            operations,
            executeBehaviors: [CommandBehavior.Throw]);
        var compound = new CompoundEditorCommand("Compound edit", [first, second]);

        var error = await Assert.ThrowsAsync<InvalidOperationException>(
            () => compound.ExecuteAsync(new ActionContext()));

        Assert.Equal("Second execute threw", error.Message);
        Assert.False(first.IsApplied);
        Assert.False(second.IsApplied);
        Assert.Equal(["execute:First", "execute:Second", "undo:First"], operations);
    }

    [Fact]
    public async Task CompoundExecute_CancelledChildUsesNonCancelledTokenForCompensation()
    {
        var operations = new List<string>();
        var first = new ScriptedStateCommand("First", operations);
        var second = new ScriptedStateCommand(
            "Second",
            operations,
            executeBehaviors: [CommandBehavior.Cancel]);
        var compound = new CompoundEditorCommand("Compound edit", [first, second]);

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => compound.ExecuteAsync(new ActionContext()));

        Assert.False(first.IsApplied);
        Assert.False(first.LastUndoTokenCanBeCancelled);
        Assert.Equal(["execute:First", "execute:Second", "undo:First"], operations);
    }

    [Fact]
    public async Task CompoundUndo_FailedChildReappliesUndoneChildrenAndCanRetry()
    {
        var operations = new List<string>();
        var first = new ScriptedStateCommand(
            "First",
            operations,
            initiallyApplied: true,
            undoBehaviors: [CommandBehavior.Failure, CommandBehavior.Success]);
        var second = new ScriptedStateCommand("Second", operations, initiallyApplied: true);
        var compound = new CompoundEditorCommand("Compound edit", [first, second]);

        var failed = await compound.UndoAsync(new ActionContext());

        Assert.False(failed.Succeeded);
        Assert.True(first.IsApplied);
        Assert.True(second.IsApplied);
        Assert.Equal(["undo:Second", "undo:First", "execute:Second"], operations);

        operations.Clear();
        var retried = await compound.UndoAsync(new ActionContext());

        Assert.True(retried.Succeeded);
        Assert.False(first.IsApplied);
        Assert.False(second.IsApplied);
        Assert.Equal(["undo:Second", "undo:First"], operations);
    }

    [Fact]
    public async Task CompoundUndo_CancelledChildReappliesUndoneChildrenWithNonCancelledToken()
    {
        var operations = new List<string>();
        var first = new ScriptedStateCommand(
            "First",
            operations,
            initiallyApplied: true,
            undoBehaviors: [CommandBehavior.Cancel]);
        var second = new ScriptedStateCommand("Second", operations, initiallyApplied: true);
        var compound = new CompoundEditorCommand("Compound edit", [first, second]);

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await compound.UndoAsync(new ActionContext()));

        Assert.True(first.IsApplied);
        Assert.True(second.IsApplied);
        Assert.False(second.LastExecuteTokenCanBeCancelled);
        Assert.Equal(["undo:Second", "undo:First", "execute:Second"], operations);
    }

    [Fact]
    public async Task CompoundUndo_CompensationFailureFaultsCommandAndPreventsUnsafeRetry()
    {
        var operations = new List<string>();
        var first = new ScriptedStateCommand(
            "First",
            operations,
            initiallyApplied: true,
            undoBehaviors: [CommandBehavior.Failure]);
        var second = new ScriptedStateCommand(
            "Second",
            operations,
            initiallyApplied: true,
            executeBehaviors: [CommandBehavior.Failure]);
        var compound = new CompoundEditorCommand("Compound edit", [first, second]);

        var failed = await compound.UndoAsync(new ActionContext());

        Assert.False(failed.Succeeded);
        Assert.Contains("Compensation also failed", failed.Message);
        Assert.Contains("cannot be retried safely", failed.Message);
        Assert.True(first.IsApplied);
        Assert.False(second.IsApplied);
        var attemptsAfterFailure = operations.Count;

        var retried = await compound.UndoAsync(new ActionContext());

        Assert.False(retried.Succeeded);
        Assert.Equal(attemptsAfterFailure, operations.Count);
    }

    [Fact]
    public async Task TransactionDispose_RollsBackActiveNestedScopesInReverseOrder()
    {
        var operations = new List<string>();
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var outerCommand = new ScriptedStateCommand("Outer", operations);
        var innerCommand = new ScriptedStateCommand("Inner", operations);
        var context = new ActionContext(Origin: new CommandOrigin(CommandOriginKind.Test, "Nested transaction"));
        var outer = await dispatcher.BeginAsync("Outer transaction", context);
        Assert.True((await dispatcher.DispatchAsync(outerCommand, context)).Succeeded);
        var inner = await dispatcher.BeginAsync("Inner transaction", context);
        Assert.True((await dispatcher.DispatchAsync(innerCommand, context)).Succeeded);

        operations.Clear();
        await outer.DisposeAsync();
        await inner.DisposeAsync();

        Assert.False(outerCommand.IsApplied);
        Assert.False(innerCommand.IsApplied);
        Assert.Equal(["undo:Inner", "undo:Outer"], operations);
        Assert.Equal(0, history.UndoCount);
        Assert.Equal(0, history.RedoCount);
    }

    [Fact]
    public async Task TransactionDispose_FailedRollbackIsExplicitAndPreservedForRetry()
    {
        var operations = new List<string>();
        var history = new InMemoryUndoRedoHistory();
        var dispatcher = new EditorCommandDispatcher(new StubContextProvider(), history);
        var command = new ScriptedStateCommand(
            "Recoverable",
            operations,
            undoBehaviors: [CommandBehavior.Failure, CommandBehavior.Success]);
        var context = new ActionContext(Origin: new CommandOrigin(CommandOriginKind.Test, "Failed rollback"));
        var transaction = await dispatcher.BeginAsync("Recoverable transaction", context);
        Assert.True((await dispatcher.DispatchAsync(command, context)).Succeeded);

        var error = await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await transaction.DisposeAsync());

        Assert.Contains("could not be rolled back", error.Message);
        Assert.Contains("preserved for explicit recovery", error.Message);
        Assert.True(command.IsApplied);
        Assert.Equal(1, history.UndoCount);

        Assert.True(await dispatcher.UndoAsync());
        Assert.False(command.IsApplied);
        Assert.Equal(0, history.UndoCount);
        Assert.Equal(1, history.RedoCount);
    }

    private sealed class StubUndoableCommand(string description) : IUndoableEditorCommand
    {
        public string Name => description;
        public string Description => description;
        public IHistoryMergePolicy? MergePolicy => null;
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
            => Task.FromResult(CommandResult.Success());
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
            => ValueTask.FromResult(CommandResult.Success());
    }

    private sealed class StubCoalescibleCommand(string target, string before, string after, string description) : IHistoryCoalescibleCommand
    {
        public string Target { get; } = target;
        public string Before { get; } = before;
        public string After { get; } = after;
        public string Name => nameof(StubCoalescibleCommand);
        public string Description { get; } = description;
        public IHistoryMergePolicy? MergePolicy => new TimeWindowHistoryMergePolicy(TimeSpan.FromSeconds(1));
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
            => Task.FromResult(CommandResult.Success());
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
            => ValueTask.FromResult(CommandResult.Success());
        public bool CanCoalesceWith(IUndoableEditorCommand next) =>
            next is StubCoalescibleCommand command && command.Target == Target;
        public IUndoableEditorCommand CoalesceWith(IUndoableEditorCommand next)
        {
            var command = (StubCoalescibleCommand)next;
            return new StubCoalescibleCommand(Target, Before, command.After, command.Description);
        }
    }

    private sealed class StubRecoverableUndoCommand : IUndoableEditorCommand
    {
        public bool AllowUndo { get; set; }
        public int UndoAttempts { get; private set; }
        public string Name => nameof(StubRecoverableUndoCommand);
        public string Description => "Recoverable edit";
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
            => Task.FromResult(CommandResult.Success());
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            UndoAttempts++;
            return ValueTask.FromResult(AllowUndo
                ? CommandResult.Success()
                : CommandResult.Failure("Undo failed"));
        }
    }

    private sealed class StubCancellableCommand(bool cancelUndo, bool cancelRedo) : IUndoableEditorCommand
    {
        int _executeCount;
        public string Name => nameof(StubCancellableCommand);
        public string Description => "Cancellable edit";
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            _executeCount++;
            return cancelRedo && _executeCount > 1
                ? Task.FromCanceled<CommandResult>(new CancellationToken(canceled: true))
                : Task.FromResult(CommandResult.Success());
        }
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
            => cancelUndo
                ? ValueTask.FromCanceled<CommandResult>(new CancellationToken(canceled: true))
                : ValueTask.FromResult(CommandResult.Success());
    }

    private sealed class BlockingUndoCommand(string description) : IUndoableEditorCommand
    {
        readonly TaskCompletionSource<bool> _undoStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
        readonly TaskCompletionSource<bool> _releaseUndo = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public string Name => description;
        public string Description => description;
        public bool IsApplied { get; private set; }
        public Task UndoStarted => _undoStarted.Task;
        public bool CanExecute(ActionContext context) => !IsApplied;

        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            IsApplied = true;
            return Task.FromResult(CommandResult.Success());
        }

        public async ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            _undoStarted.TrySetResult(true);
            await _releaseUndo.Task.WaitAsync(cancellationToken);
            IsApplied = false;
            return CommandResult.Success();
        }

        public void ReleaseUndo() => _releaseUndo.TrySetResult(true);
    }

    private sealed class StateTrackingCommand(string description) : IUndoableEditorCommand
    {
        public string Name => description;
        public string Description => description;
        public int ExecuteCount { get; private set; }
        public int UndoCount { get; private set; }
        public bool IsApplied { get; private set; }
        public bool CanExecute(ActionContext context) => !IsApplied;

        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            ExecuteCount++;
            IsApplied = true;
            return Task.FromResult(CommandResult.Success());
        }

        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            UndoCount++;
            IsApplied = false;
            return ValueTask.FromResult(CommandResult.Success());
        }
    }

    private enum CommandBehavior
    {
        Success,
        Failure,
        Cancel,
        Throw,
    }

    private sealed class ScriptedStateCommand : IUndoableEditorCommand
    {
        readonly Queue<CommandBehavior> _executeBehaviors;
        readonly Queue<CommandBehavior> _undoBehaviors;
        readonly List<string> _operations;

        public ScriptedStateCommand(
            string name,
            List<string> operations,
            bool initiallyApplied = false,
            IEnumerable<CommandBehavior>? executeBehaviors = null,
            IEnumerable<CommandBehavior>? undoBehaviors = null)
        {
            Name = name;
            Description = name;
            IsApplied = initiallyApplied;
            _operations = operations;
            _executeBehaviors = new Queue<CommandBehavior>(executeBehaviors ?? []);
            _undoBehaviors = new Queue<CommandBehavior>(undoBehaviors ?? []);
        }

        public string Name { get; }
        public string Description { get; }
        public bool IsApplied { get; private set; }
        public bool LastExecuteTokenCanBeCancelled { get; private set; }
        public bool LastUndoTokenCanBeCancelled { get; private set; }
        public bool CanExecute(ActionContext context) => !IsApplied;

        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            LastExecuteTokenCanBeCancelled = cancellationToken.CanBeCanceled;
            _operations.Add($"execute:{Name}");
            var behavior = NextBehavior(_executeBehaviors);
            if (behavior == CommandBehavior.Cancel)
                return Task.FromCanceled<CommandResult>(new CancellationToken(canceled: true));
            if (behavior == CommandBehavior.Throw)
                throw new InvalidOperationException($"{Name} execute threw");
            if (behavior == CommandBehavior.Failure)
                return Task.FromResult(CommandResult.Failure($"{Name} execute failed"));
            if (IsApplied)
                return Task.FromResult(CommandResult.Failure($"{Name} was applied twice"));

            IsApplied = true;
            return Task.FromResult(CommandResult.Success());
        }

        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            LastUndoTokenCanBeCancelled = cancellationToken.CanBeCanceled;
            _operations.Add($"undo:{Name}");
            var behavior = NextBehavior(_undoBehaviors);
            if (behavior == CommandBehavior.Cancel)
                return ValueTask.FromCanceled<CommandResult>(new CancellationToken(canceled: true));
            if (behavior == CommandBehavior.Throw)
                throw new InvalidOperationException($"{Name} undo threw");
            if (behavior == CommandBehavior.Failure)
                return ValueTask.FromResult(CommandResult.Failure($"{Name} undo failed"));
            if (!IsApplied)
                return ValueTask.FromResult(CommandResult.Failure($"{Name} was undone twice"));

            IsApplied = false;
            return ValueTask.FromResult(CommandResult.Success());
        }

        static CommandBehavior NextBehavior(Queue<CommandBehavior> behaviors) =>
            behaviors.Count == 0 ? CommandBehavior.Success : behaviors.Dequeue();
    }

    private sealed class StubContextProvider : IActionContextProvider
    {
        public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null)
            => new(Origin: origin, Metadata: metadata);
    }
}
