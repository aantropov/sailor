using SailorEditor.History;

namespace SailorEditor.Commands;

public interface ICommandHistoryService
{
    long WorkspaceEpoch { get; }
    int UndoCount { get; }
    int RedoCount { get; }
    string? UndoLabel { get; }
    string? RedoLabel { get; }
    bool CanUndo { get; }
    bool CanRedo { get; }
    bool IsWorkspaceChangeInProgress { get; }
    Task<bool> UndoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default);
    Task<bool> RedoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default);
    void BeginWorkspaceChange();
    Task BeginWorkspaceChangeAsync(CancellationToken cancellationToken = default);
    void ResetForWorkspaceChange();
    void CompleteWorkspaceChange();
}

public sealed class EditorCommandDispatcher : ICommandDispatcher, ICommandHistoryService, ITransactionScopeFactory
{
    readonly IActionContextProvider _contextProvider;
    readonly IUndoRedoHistory _history;
    readonly Stack<TransactionScope> _transactions = new();
    readonly object _workspaceStateLock = new();
    CancellationTokenSource _workspaceCancellation = new();
    TaskCompletionSource<bool>? _executionDrain;
    long _workspaceEpoch;
    int _inFlightExecutionCount;
    bool _workspaceChangeInProgress;
    bool _transactionRollbackInProgress;
    bool _isUndoRedo;

    public EditorCommandDispatcher(IActionContextProvider contextProvider, IUndoRedoHistory history)
    {
        _contextProvider = contextProvider;
        _history = history;
    }

    public long WorkspaceEpoch => Interlocked.Read(ref _workspaceEpoch);
    public int UndoCount
    {
        get
        {
            lock (_workspaceStateLock)
                return _history.UndoCount;
        }
    }
    public int RedoCount
    {
        get
        {
            lock (_workspaceStateLock)
                return _history.RedoCount;
        }
    }
    public string? UndoLabel
    {
        get
        {
            lock (_workspaceStateLock)
                return _history.PeekUndo()?.Description;
        }
    }
    public string? RedoLabel
    {
        get
        {
            lock (_workspaceStateLock)
                return _history.PeekRedo()?.Description;
        }
    }
    public bool CanUndo => UndoCount > 0;
    public bool CanRedo => RedoCount > 0;
    public bool IsWorkspaceChangeInProgress
    {
        get
        {
            lock (_workspaceStateLock)
                return _workspaceChangeInProgress;
        }
    }

    public async Task<CommandResult> DispatchAsync(IEditorCommand command, ActionContext context, CancellationToken cancellationToken = default)
    {
        (long Epoch, CancellationTokenSource Cancellation) execution;
        lock (_workspaceStateLock)
        {
            if (_workspaceChangeInProgress)
                return WorkspaceChangingFailure(command.Name);
            if (_transactionRollbackInProgress)
                return TransactionRollbackFailure(command.Name);
            if (_isUndoRedo)
                return HistoryOperationInProgressFailure(command.Name);
            execution = BeginExecutionUnderLock(cancellationToken);
        }

        try
        {
            using var linkedCancellation = execution.Cancellation;
            if (context.WorkspaceEpoch is { } contextEpoch && contextEpoch != execution.Epoch)
                return WorkspaceChangedFailure(command.Name);
            if (!command.CanExecute(context))
                return CommandResult.Failure($"Command '{command.Name}' cannot execute.");

            CommandResult result;
            try
            {
                result = await command.ExecuteAsync(context, linkedCancellation.Token);
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested && !IsCurrentEpoch(execution.Epoch))
            {
                return WorkspaceChangedFailure(command.Name);
            }

            lock (_workspaceStateLock)
            {
                if (!IsCurrentEpoch(execution.Epoch))
                    return WorkspaceChangedFailure(command.Name);

                if (!result.Succeeded || command is not IUndoableEditorCommand undoable || _isUndoRedo)
                    return result;

                var entry = new HistoryEntry(undoable.Description, undoable, context.Origin, DateTimeOffset.UtcNow, context.Metadata);
                if (_transactions.TryPeek(out var transaction) && transaction.WorkspaceEpoch == execution.Epoch)
                    transaction.Add(entry);
                else
                    PushWithMerge(entry);
            }

            return result;
        }
        finally
        {
            CompleteExecution();
        }
    }

    public ValueTask<ITransactionScope> BeginAsync(string description, ActionContext context, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_workspaceStateLock)
        {
            if (_workspaceChangeInProgress)
                throw new InvalidOperationException("A command transaction cannot begin while the active workspace is changing.");
            if (_transactionRollbackInProgress)
                throw new InvalidOperationException("A command transaction cannot begin while another transaction is rolling back.");
            if (context.WorkspaceEpoch is { } contextEpoch && contextEpoch != WorkspaceEpoch)
                throw new InvalidOperationException("A command transaction cannot begin with context from a previous workspace.");

            var scope = new TransactionScope(this, description, context, WorkspaceEpoch);
            _transactions.Push(scope);
            return ValueTask.FromResult<ITransactionScope>(scope);
        }
    }

    public async Task<bool> UndoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default)
    {
        HistoryEntry entry;
        (long Epoch, CancellationTokenSource Cancellation) execution;
        lock (_workspaceStateLock)
        {
            if (_workspaceChangeInProgress || _transactionRollbackInProgress || _isUndoRedo || _history.UndoCount == 0)
                return false;

            execution = BeginExecutionUnderLock(cancellationToken);
            entry = _history.PopUndo();
            _isUndoRedo = true;
        }
        using var linkedCancellation = execution.Cancellation;

        try
        {
            if (!IsCurrentEpoch(execution.Epoch))
                return false;

            var context = _contextProvider.GetCurrentContext(
                origin ?? new CommandOrigin(CommandOriginKind.Menu, "Undo"),
                entry.Metadata);
            var result = await entry.Command.UndoAsync(context, linkedCancellation.Token);
            lock (_workspaceStateLock)
            {
                if (!IsCurrentEpoch(execution.Epoch))
                    return false;

                if (!result.Succeeded)
                {
                    _history.PushUndoPreservingRedo(entry);
                    return false;
                }

                _history.PushRedo(entry);
                return true;
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested && !IsCurrentEpoch(execution.Epoch))
        {
            return false;
        }
        catch (OperationCanceledException)
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _history.PushUndoPreservingRedo(entry);
            }

            return false;
        }
        catch
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _history.PushUndoPreservingRedo(entry);
            }

            throw;
        }
        finally
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _isUndoRedo = false;
            }
            CompleteExecution();
        }
    }

    public async Task<bool> RedoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default)
    {
        HistoryEntry entry;
        (long Epoch, CancellationTokenSource Cancellation) execution;
        lock (_workspaceStateLock)
        {
            if (_workspaceChangeInProgress || _transactionRollbackInProgress || _isUndoRedo || _history.RedoCount == 0)
                return false;

            execution = BeginExecutionUnderLock(cancellationToken);
            entry = _history.PopRedo();
            _isUndoRedo = true;
        }
        using var linkedCancellation = execution.Cancellation;

        try
        {
            if (!IsCurrentEpoch(execution.Epoch))
                return false;

            var context = _contextProvider.GetCurrentContext(
                origin ?? new CommandOrigin(CommandOriginKind.Menu, "Redo"),
                entry.Metadata);
            var result = await entry.Command.ExecuteAsync(context, linkedCancellation.Token);
            lock (_workspaceStateLock)
            {
                if (!IsCurrentEpoch(execution.Epoch))
                    return false;

                if (!result.Succeeded)
                {
                    _history.PushRedo(entry);
                    return false;
                }

                _history.PushUndoPreservingRedo(entry);
                return true;
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested && !IsCurrentEpoch(execution.Epoch))
        {
            return false;
        }
        catch (OperationCanceledException)
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _history.PushRedo(entry);
            }

            return false;
        }
        catch
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _history.PushRedo(entry);
            }

            throw;
        }
        finally
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _isUndoRedo = false;
            }
            CompleteExecution();
        }
    }

    public void BeginWorkspaceChange()
    {
        var transition = BeginWorkspaceChangeUnderLock();
        CancelAndDispose(transition.PreviousCancellation);
    }

    public async Task BeginWorkspaceChangeAsync(CancellationToken cancellationToken = default)
    {
        var transition = BeginWorkspaceChangeUnderLock();
        Exception? cancellationFailure = null;
        try
        {
            CancelAndDispose(transition.PreviousCancellation);
        }
        catch (Exception exception)
        {
            cancellationFailure = exception;
        }

        await transition.ExecutionDrain.WaitAsync(cancellationToken).ConfigureAwait(false);
        if (cancellationFailure is not null)
        {
            throw new InvalidOperationException(
                "The previous workspace command cancellation reported an error after its executions drained.",
                cancellationFailure);
        }
    }

    public void ResetForWorkspaceChange()
    {
        BeginWorkspaceChange();
        lock (_workspaceStateLock)
        {
            if (_inFlightExecutionCount != 0)
            {
                throw new InvalidOperationException(
                    "Command history cannot reset until active command executions have drained. Await BeginWorkspaceChangeAsync first.");
            }

            _transactions.Clear();
            _isUndoRedo = false;
            _history.Clear();
        }
    }

    public void CompleteWorkspaceChange()
    {
        lock (_workspaceStateLock)
        {
            if (_inFlightExecutionCount != 0)
            {
                throw new InvalidOperationException(
                    "Workspace command dispatch cannot resume until prior command executions have drained.");
            }

            _workspaceChangeInProgress = false;
        }
    }

    (CancellationTokenSource? PreviousCancellation, Task ExecutionDrain) BeginWorkspaceChangeUnderLock()
    {
        lock (_workspaceStateLock)
        {
            CancellationTokenSource? previousCancellation = null;
            if (!_workspaceChangeInProgress)
            {
                _workspaceChangeInProgress = true;
                Interlocked.Increment(ref _workspaceEpoch);
                previousCancellation = _workspaceCancellation;
                _workspaceCancellation = new CancellationTokenSource();
                _transactions.Clear();
                _isUndoRedo = false;
            }

            var executionDrain = _inFlightExecutionCount == 0
                ? Task.CompletedTask
                : _executionDrain?.Task
                    ?? throw new InvalidOperationException("Active command executions are missing their drain signal.");
            return (previousCancellation, executionDrain);
        }
    }

    (long Epoch, CancellationTokenSource Cancellation) BeginExecutionUnderLock(CancellationToken cancellationToken)
    {
        var cancellation = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _workspaceCancellation.Token);
        RegisterExecutionUnderLock();
        return (WorkspaceEpoch, cancellation);
    }

    void RegisterExecutionUnderLock()
    {
        if (_inFlightExecutionCount == 0)
        {
            _executionDrain = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        }

        _inFlightExecutionCount++;
    }

    void CompleteExecution()
    {
        TaskCompletionSource<bool>? completedDrain = null;
        lock (_workspaceStateLock)
        {
            if (_inFlightExecutionCount <= 0)
                throw new InvalidOperationException("Command execution accounting underflowed.");

            _inFlightExecutionCount--;
            if (_inFlightExecutionCount == 0)
            {
                completedDrain = _executionDrain
                    ?? throw new InvalidOperationException("Command execution completed without a drain signal.");
                _executionDrain = null;
            }
        }

        completedDrain?.TrySetResult(true);
    }

    static void CancelAndDispose(CancellationTokenSource? cancellation)
    {
        if (cancellation is null)
            return;

        try
        {
            cancellation.Cancel();
        }
        finally
        {
            cancellation.Dispose();
        }
    }

    bool IsCurrentEpoch(long epoch) => epoch == WorkspaceEpoch;

    static CommandResult WorkspaceChangedFailure(string commandName) =>
        CommandResult.Failure($"Command '{commandName}' was discarded because the active workspace changed.");

    static CommandResult WorkspaceChangingFailure(string commandName) =>
        CommandResult.Failure($"Command '{commandName}' was discarded because workspace activation is in progress.");

    static CommandResult TransactionRollbackFailure(string commandName) =>
        CommandResult.Failure($"Command '{commandName}' was discarded because a command transaction is rolling back.");

    static CommandResult HistoryOperationInProgressFailure(string commandName) =>
        CommandResult.Failure($"Command '{commandName}' was discarded because an undo or redo operation is in progress.");

    void PushWithMerge(HistoryEntry entry)
    {
        var previous = _history.PeekUndo();
        if (_history.RedoCount == 0 &&
            entry.Command.MergePolicy is { } policy &&
            previous is not null &&
            policy.CanMerge(previous, entry))
        {
            var merged = policy.Merge(previous, entry);
            _history.PopUndo();
            _history.Push(merged);
            return;
        }

        _history.Push(entry);
    }

    void Complete(TransactionScope scope)
    {
        lock (_workspaceStateLock)
        {
            if (!IsCurrentEpoch(scope.WorkspaceEpoch) || _transactions.Count == 0 || !ReferenceEquals(scope, _transactions.Peek()))
                return;

            var popped = _transactions.Pop();
            if (!ReferenceEquals(scope, popped) || scope.Entries.Count == 0)
                return;

            var command = scope.Entries.Count == 1
                ? scope.Entries[0].Command
                : new CompoundEditorCommand(scope.Description, scope.Entries.Select(x => x.Command).ToArray());

            var entry = new HistoryEntry(scope.Description, command, scope.Context.Origin, DateTimeOffset.UtcNow, scope.Context.Metadata);
            if (_transactions.TryPeek(out var parent))
                parent.Add(entry);
            else
                PushWithMerge(entry);
        }
    }

    async ValueTask CancelAsync(TransactionScope scope)
    {
        HistoryEntry[] entries;
        lock (_workspaceStateLock)
        {
            if (!IsCurrentEpoch(scope.WorkspaceEpoch))
            {
                scope.MarkFinalizedByOwner();
                return;
            }

            var activeScopes = _transactions.ToArray();
            var scopeIndex = Array.FindIndex(activeScopes, activeScope => ReferenceEquals(activeScope, scope));
            if (scopeIndex < 0)
            {
                scope.MarkFinalizedByOwner();
                return;
            }

            var scopesToCancel = activeScopes.Take(scopeIndex + 1).ToArray();
            foreach (var expectedScope in scopesToCancel)
            {
                var poppedScope = _transactions.Pop();
                if (!ReferenceEquals(expectedScope, poppedScope))
                    throw new InvalidOperationException("Command transaction nesting changed while cancellation was in progress.");

                poppedScope.MarkFinalizedByOwner();
            }

            entries = scopesToCancel
                .Reverse()
                .SelectMany(cancelledScope => cancelledScope.Entries)
                .ToArray();

            if (entries.Length == 0)
                return;

            _transactionRollbackInProgress = true;
            RegisterExecutionUnderLock();
        }

        var rollbackCommand = entries.Length == 1
            ? entries[0].Command
            : new CompoundEditorCommand(scope.Description, entries.Select(entry => entry.Command).ToArray());

        try
        {
            CommandResult? rollbackResult = null;
            Exception? rollbackException = null;
            try
            {
                rollbackResult = await rollbackCommand.UndoAsync(scope.Context, CancellationToken.None);
            }
            catch (Exception exception)
            {
                rollbackException = exception;
            }

            var rollbackFailed = rollbackException is not null || rollbackResult is not { Succeeded: true };
            lock (_workspaceStateLock)
            {
                try
                {
                    if (rollbackFailed && IsCurrentEpoch(scope.WorkspaceEpoch))
                    {
                        var preservedEntry = new HistoryEntry(
                            scope.Description,
                            rollbackCommand,
                            scope.Context.Origin,
                            DateTimeOffset.UtcNow,
                            scope.Context.Metadata);
                        if (_transactions.TryPeek(out var parent))
                            parent.Add(preservedEntry);
                        else
                            _history.Push(preservedEntry);
                    }
                }
                finally
                {
                    _transactionRollbackInProgress = false;
                }
            }

            if (rollbackException is not null)
            {
                throw new InvalidOperationException(
                    $"Transaction '{scope.Description}' could not be rolled back. Its history entry was preserved for explicit recovery.",
                    rollbackException);
            }

            if (rollbackResult is not { Succeeded: true })
            {
                var detail = string.IsNullOrWhiteSpace(rollbackResult?.Message)
                    ? "A child command rejected the rollback."
                    : rollbackResult.Message;
                throw new InvalidOperationException(
                    $"Transaction '{scope.Description}' could not be rolled back: {detail} Its history entry was preserved for explicit recovery.");
            }
        }
        finally
        {
            CompleteExecution();
        }
    }

    sealed class TransactionScope(EditorCommandDispatcher owner, string description, ActionContext context, long workspaceEpoch) : ITransactionScope
    {
        bool _finalized;
        public string Description { get; } = description;
        public ActionContext Context { get; } = context;
        public long WorkspaceEpoch { get; } = workspaceEpoch;
        public List<HistoryEntry> Entries { get; } = [];
        public void Add(HistoryEntry entry) => Entries.Add(entry);
        public ValueTask CompleteAsync(CancellationToken cancellationToken = default)
        {
            if (_finalized)
                return ValueTask.CompletedTask;

            cancellationToken.ThrowIfCancellationRequested();
            owner.Complete(this);
            _finalized = true;
            return ValueTask.CompletedTask;
        }
        public async ValueTask DisposeAsync()
        {
            if (_finalized)
                return;

            _finalized = true;
            await owner.CancelAsync(this);
        }

        public void MarkFinalizedByOwner() => _finalized = true;
    }
}

public sealed class CompoundEditorCommand(string description, IReadOnlyList<IUndoableEditorCommand> commands) : IUndoableEditorCommand
{
    readonly object _faultLock = new();
    string? _faultMessage;

    public string Name => description;
    public string Description => description;
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => commands.All(x => x.CanExecute(context));

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (GetFaultResult() is { } faultResult)
            return faultResult;

        var executedCommands = new List<IUndoableEditorCommand>(commands.Count);
        try
        {
            foreach (var command in commands)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var result = await command.ExecuteAsync(context, cancellationToken);
                if (!result.Succeeded)
                {
                    var compensation = await UndoCommandsAsync(executedCommands, context);
                    return compensation.Succeeded
                        ? result
                        : MarkFaulted(CreateCompensationFailure("execution", command, result.Message, compensation.Message));
                }

                executedCommands.Add(command);
            }
        }
        catch (Exception exception)
        {
            var compensation = await UndoCommandsAsync(executedCommands, context);
            if (!compensation.Succeeded)
            {
                throw MarkFaulted(
                    CreateCompensationFailure("execution", null, exception.Message, compensation.Message),
                    exception);
            }

            throw;
        }

        return CommandResult.Success();
    }

    public async ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (GetFaultResult() is { } faultResult)
            return faultResult;

        var undoneCommands = new List<IUndoableEditorCommand>(commands.Count);
        try
        {
            for (var i = commands.Count - 1; i >= 0; i--)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var command = commands[i];
                var result = await command.UndoAsync(context, cancellationToken);
                if (!result.Succeeded)
                {
                    var compensation = await ExecuteCommandsInOriginalOrderAsync(undoneCommands, context);
                    return compensation.Succeeded
                        ? result
                        : MarkFaulted(CreateCompensationFailure("undo", command, result.Message, compensation.Message));
                }

                undoneCommands.Add(command);
            }
        }
        catch (Exception exception)
        {
            var compensation = await ExecuteCommandsInOriginalOrderAsync(undoneCommands, context);
            if (!compensation.Succeeded)
            {
                throw MarkFaulted(
                    CreateCompensationFailure("undo", null, exception.Message, compensation.Message),
                    exception);
            }

            throw;
        }

        return CommandResult.Success();
    }

    CommandResult? GetFaultResult()
    {
        lock (_faultLock)
            return _faultMessage is null ? null : CommandResult.Failure(_faultMessage);
    }

    CommandResult MarkFaulted(string message)
    {
        lock (_faultLock)
            _faultMessage ??= message;
        return CommandResult.Failure(_faultMessage);
    }

    Exception MarkFaulted(string message, Exception innerException)
    {
        lock (_faultLock)
            _faultMessage ??= message;
        return new InvalidOperationException(_faultMessage, innerException);
    }

    static async Task<CommandResult> UndoCommandsAsync(
        IReadOnlyList<IUndoableEditorCommand> executedCommands,
        ActionContext context)
    {
        var failures = new List<string>();
        for (var i = executedCommands.Count - 1; i >= 0; i--)
        {
            var command = executedCommands[i];
            try
            {
                var result = await command.UndoAsync(context, CancellationToken.None);
                if (!result.Succeeded)
                    failures.Add(DescribeFailure(command, result.Message));
            }
            catch (Exception exception)
            {
                failures.Add(DescribeFailure(command, exception.Message));
            }
        }

        return failures.Count == 0
            ? CommandResult.Success()
            : CommandResult.Failure(string.Join("; ", failures));
    }

    static async Task<CommandResult> ExecuteCommandsInOriginalOrderAsync(
        IReadOnlyList<IUndoableEditorCommand> undoneCommands,
        ActionContext context)
    {
        var failures = new List<string>();
        for (var i = undoneCommands.Count - 1; i >= 0; i--)
        {
            var command = undoneCommands[i];
            try
            {
                var result = await command.ExecuteAsync(context, CancellationToken.None);
                if (!result.Succeeded)
                    failures.Add(DescribeFailure(command, result.Message));
            }
            catch (Exception exception)
            {
                failures.Add(DescribeFailure(command, exception.Message));
            }
        }

        return failures.Count == 0
            ? CommandResult.Success()
            : CommandResult.Failure(string.Join("; ", failures));
    }

    static string DescribeFailure(IUndoableEditorCommand command, string? message) =>
        string.IsNullOrWhiteSpace(message)
            ? $"Command '{command.Name}' rejected compensation"
            : $"Command '{command.Name}' rejected compensation: {message}";

    static string CreateCompensationFailure(
        string operation,
        IUndoableEditorCommand? failedCommand,
        string? originalFailure,
        string? compensationFailure)
    {
        var commandName = failedCommand is null ? "a child command" : $"command '{failedCommand.Name}'";
        var originalDetail = string.IsNullOrWhiteSpace(originalFailure) ? "without a diagnostic" : originalFailure;
        var compensationDetail = string.IsNullOrWhiteSpace(compensationFailure)
            ? "without a diagnostic"
            : compensationFailure;
        return $"Compound {operation} failed in {commandName}: {originalDetail}. " +
            $"Compensation also failed: {compensationDetail}. The compound command is faulted and cannot be retried safely.";
    }
}

public sealed class TimeWindowHistoryMergePolicy(TimeSpan window) : IHistoryMergePolicy
{
    public bool CanMerge(HistoryEntry previous, HistoryEntry next) =>
        next.EffectiveTimestamp >= previous.EffectiveTimestamp &&
        next.EffectiveTimestamp - previous.EffectiveTimestamp <= window &&
        previous.Command is IHistoryCoalescibleCommand coalescible &&
        coalescible.CanCoalesceWith(next.Command);

    public HistoryEntry Merge(HistoryEntry previous, HistoryEntry next)
    {
        var coalescible = (IHistoryCoalescibleCommand)previous.Command;
        return next with { Command = coalescible.CoalesceWith(next.Command) };
    }
}
