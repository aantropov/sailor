using SailorEditor.History;
using SailorEditor.Services;
using SailorEditor.State;
using SailorEditor.ViewModels;
using SailorEngine;

namespace SailorEditor.Commands;

public sealed class EditorActionContextProvider(WorldService worldService, SelectionService selectionService, ShellState shellState) : IActionContextProvider
{
    public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null)
    {
        var selectedIds = selectionService.SelectedItems
            .Select(TryGetSelectionId)
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Cast<string>()
            .ToArray();

        return new ActionContext(
            ActiveWorldId: worldService.Current?.Name,
            ActiveSelectionIds: selectedIds,
            ActivePanelId: shellState.Focus.FocusedPanelId?.Value,
            ActiveDocumentId: shellState.Focus.ActiveDocumentPanelId?.Value,
            FocusedViewportId: shellState.Focus.FocusedViewportId,
            CurrentAssetId: TryGetSelectionId(selectionService.SelectedItem),
            IsPlayMode: false,
            Origin: origin,
            Metadata: metadata);
    }

    static string? TryGetSelectionId(object? selected) => selected switch
    {
        GameObject go => go.InstanceId?.Value,
        Component component => component.InstanceId?.Value,
        _ => null,
    };
}

public interface ICommandHistoryService
{
    int UndoCount { get; }
    int RedoCount { get; }
    string? UndoLabel { get; }
    string? RedoLabel { get; }
    bool CanUndo { get; }
    bool CanRedo { get; }
    Task<bool> UndoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default);
    Task<bool> RedoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default);
}

public sealed class EditorCommandDispatcher : ICommandDispatcher, ICommandHistoryService, ITransactionScopeFactory
{
    readonly IActionContextProvider _contextProvider;
    readonly IUndoRedoHistory _history;
    readonly Stack<TransactionScope> _transactions = new();
    bool _isUndoRedo;

    public EditorCommandDispatcher(IActionContextProvider contextProvider, IUndoRedoHistory history)
    {
        _contextProvider = contextProvider;
        _history = history;
    }

    public int UndoCount => _history.UndoCount;
    public int RedoCount => _history.RedoCount;
    public string? UndoLabel => _history.PeekUndo()?.Description;
    public string? RedoLabel => _history.PeekRedo()?.Description;
    public bool CanUndo => UndoCount > 0;
    public bool CanRedo => RedoCount > 0;

    public async Task<CommandResult> DispatchAsync(IEditorCommand command, ActionContext context, CancellationToken cancellationToken = default)
    {
        if (!command.CanExecute(context))
            return CommandResult.Failure($"Command '{command.Name}' cannot execute.");

        var result = await command.ExecuteAsync(context, cancellationToken);
        if (!result.Succeeded || command is not IUndoableEditorCommand undoable || _isUndoRedo)
            return result;

        var entry = new HistoryEntry(undoable.Description, undoable, context.Origin, DateTimeOffset.UtcNow, context.Metadata);
        if (_transactions.TryPeek(out var transaction))
        {
            transaction.Add(entry);
        }
        else
        {
            PushWithMerge(entry);
        }

        return result;
    }

    public ValueTask<ITransactionScope> BeginAsync(string description, ActionContext context, CancellationToken cancellationToken = default)
    {
        var scope = new TransactionScope(this, description, context);
        _transactions.Push(scope);
        return ValueTask.FromResult<ITransactionScope>(scope);
    }

    public async Task<bool> UndoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default)
    {
        if (!CanUndo)
            return false;

        var entry = _history.PopUndo();
        var context = _contextProvider.GetCurrentContext(origin ?? new CommandOrigin(CommandOriginKind.Menu, "Undo"), entry.Metadata);

        _isUndoRedo = true;
        try
        {
            await entry.Command.UndoAsync(context, cancellationToken);
            _history.PushRedo(entry);
            return true;
        }
        finally
        {
            _isUndoRedo = false;
        }
    }

    public async Task<bool> RedoAsync(CommandOrigin? origin = null, CancellationToken cancellationToken = default)
    {
        if (!CanRedo)
            return false;

        var entry = _history.PopRedo();
        var context = _contextProvider.GetCurrentContext(origin ?? new CommandOrigin(CommandOriginKind.Menu, "Redo"), entry.Metadata);

        _isUndoRedo = true;
        try
        {
            var result = await entry.Command.ExecuteAsync(context, cancellationToken);
            if (!result.Succeeded)
                return false;

            _history.Push(entry);
            return true;
        }
        finally
        {
            _isUndoRedo = false;
        }
    }

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

    void Cancel(TransactionScope scope)
    {
        if (_transactions.Count > 0 && ReferenceEquals(_transactions.Peek(), scope))
            _transactions.Pop();
    }

    sealed class TransactionScope(EditorCommandDispatcher owner, string description, ActionContext context) : ITransactionScope
    {
        bool _completed;
        public string Description { get; } = description;
        public ActionContext Context { get; } = context;
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
