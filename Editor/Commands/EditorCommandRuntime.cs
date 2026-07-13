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
    long _workspaceEpoch;
    bool _workspaceChangeInProgress;
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
            execution = CaptureExecutionUnderLock(cancellationToken);
        }
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

    public ValueTask<ITransactionScope> BeginAsync(string description, ActionContext context, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_workspaceStateLock)
        {
            if (_workspaceChangeInProgress)
                throw new InvalidOperationException("A command transaction cannot begin while the active workspace is changing.");
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
            if (_workspaceChangeInProgress || _history.UndoCount == 0)
                return false;

            execution = CaptureExecutionUnderLock(cancellationToken);
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
            await entry.Command.UndoAsync(context, linkedCancellation.Token);
            lock (_workspaceStateLock)
            {
                if (!IsCurrentEpoch(execution.Epoch))
                    return false;

                _history.PushRedo(entry);
                return true;
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested && !IsCurrentEpoch(execution.Epoch))
        {
            return false;
        }
        finally
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _isUndoRedo = false;
            }
        }
    }

    public async Task<bool> RedoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default)
    {
        HistoryEntry entry;
        (long Epoch, CancellationTokenSource Cancellation) execution;
        lock (_workspaceStateLock)
        {
            if (_workspaceChangeInProgress || _history.RedoCount == 0)
                return false;

            execution = CaptureExecutionUnderLock(cancellationToken);
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
                if (!IsCurrentEpoch(execution.Epoch) || !result.Succeeded)
                    return false;

                _history.Push(entry);
                return true;
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested && !IsCurrentEpoch(execution.Epoch))
        {
            return false;
        }
        finally
        {
            lock (_workspaceStateLock)
            {
                if (IsCurrentEpoch(execution.Epoch))
                    _isUndoRedo = false;
            }
        }
    }

    public void BeginWorkspaceChange()
    {
        CancellationTokenSource? previousCancellation = null;
        lock (_workspaceStateLock)
        {
            if (_workspaceChangeInProgress)
                return;

            _workspaceChangeInProgress = true;
            Interlocked.Increment(ref _workspaceEpoch);
            previousCancellation = _workspaceCancellation;
            _workspaceCancellation = new CancellationTokenSource();
            _transactions.Clear();
            _isUndoRedo = false;
        }

        previousCancellation.Cancel();
        previousCancellation.Dispose();
    }

    public void ResetForWorkspaceChange()
    {
        BeginWorkspaceChange();
        lock (_workspaceStateLock)
        {
            _transactions.Clear();
            _isUndoRedo = false;
            _history.Clear();
        }
    }

    public void CompleteWorkspaceChange()
    {
        lock (_workspaceStateLock)
            _workspaceChangeInProgress = false;
    }

    (long Epoch, CancellationTokenSource Cancellation) CaptureExecution(CancellationToken cancellationToken)
    {
        lock (_workspaceStateLock)
            return CaptureExecutionUnderLock(cancellationToken);
    }

    (long Epoch, CancellationTokenSource Cancellation) CaptureExecutionUnderLock(CancellationToken cancellationToken)
        => (WorkspaceEpoch, CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _workspaceCancellation.Token));

    bool IsCurrentEpoch(long epoch) => epoch == WorkspaceEpoch;

    static CommandResult WorkspaceChangedFailure(string commandName) =>
        CommandResult.Failure($"Command '{commandName}' was discarded because the active workspace changed.");

    static CommandResult WorkspaceChangingFailure(string commandName) =>
        CommandResult.Failure($"Command '{commandName}' was discarded because workspace activation is in progress.");

    void PushWithMerge(HistoryEntry entry)
    {
        var previous = _history.PeekUndo();
        var merged = entry.Command.MergePolicy is { } policy && previous is not null && policy.CanMerge(previous, entry)
            ? policy.Merge(previous, entry)
            : entry;

        if (!ReferenceEquals(merged, entry) && previous is not null)
            _history.PopUndo();

        _history.Push(merged);
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

    void Cancel(TransactionScope scope)
    {
        lock (_workspaceStateLock)
        {
            if (IsCurrentEpoch(scope.WorkspaceEpoch) && _transactions.Count > 0 && ReferenceEquals(_transactions.Peek(), scope))
                _transactions.Pop();
        }
    }

    sealed class TransactionScope(EditorCommandDispatcher owner, string description, ActionContext context, long workspaceEpoch) : ITransactionScope
    {
        bool _completed;
        public string Description { get; } = description;
        public ActionContext Context { get; } = context;
        public long WorkspaceEpoch { get; } = workspaceEpoch;
        public List<HistoryEntry> Entries { get; } = [];
        public void Add(HistoryEntry entry) => Entries.Add(entry);
        public ValueTask CompleteAsync(CancellationToken cancellationToken = default)
        {
            if (_completed)
                return ValueTask.CompletedTask;
            _completed = true;
            owner.Complete(this);
            return ValueTask.CompletedTask;
        }
        public async ValueTask DisposeAsync()
        {
            if (!_completed)
                owner.Cancel(this);
            await ValueTask.CompletedTask;
        }
    }
}

public sealed class CompoundEditorCommand(string description, IReadOnlyList<IUndoableEditorCommand> commands) : IUndoableEditorCommand
{
    public string Name => description;
    public string Description => description;
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => commands.All(x => x.CanExecute(context));
    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        foreach (var command in commands)
        {
            var result = await command.ExecuteAsync(context, cancellationToken);
            if (!result.Succeeded)
                return result;
        }
        return CommandResult.Success();
    }
    public async ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        for (var i = commands.Count - 1; i >= 0; i--)
            await commands[i].UndoAsync(context, cancellationToken);
    }
}

public sealed class TimeWindowHistoryMergePolicy(TimeSpan window) : IHistoryMergePolicy
{
    public bool CanMerge(HistoryEntry previous, HistoryEntry next) =>
        previous.Description == next.Description && next.EffectiveTimestamp - previous.EffectiveTimestamp <= window;

    public HistoryEntry Merge(HistoryEntry previous, HistoryEntry next) => next;
}
