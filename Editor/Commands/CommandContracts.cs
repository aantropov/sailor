using SailorEditor.History;

namespace SailorEditor.Commands;

public enum CommandOriginKind
{
    Unknown,
    UI,
    Hotkey,
    Menu,
    DragDrop,
    Panel,
    AI,
    Script,
    Test,
}

public sealed record CommandOrigin(CommandOriginKind Kind, string? Source = null, string? Actor = null);

public sealed record ActionContext(
    string? ActiveWorldId = null,
    IReadOnlyList<string>? ActiveSelectionIds = null,
    string? ActivePanelId = null,
    string? ActiveDocumentId = null,
    string? FocusedViewportId = null,
    string? CurrentAssetId = null,
    bool IsPlayMode = false,
    CommandOrigin? Origin = null,
    IReadOnlyDictionary<string, string?>? Metadata = null);

public interface IEditorCommand
{
    string Name { get; }

    bool CanExecute(ActionContext context);

    Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default);
}

public interface IUndoableEditorCommand : IEditorCommand
{
    string Description { get; }

    ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default);

    IHistoryMergePolicy? MergePolicy => null;
}

public sealed record CommandResult(bool Succeeded, string? Message = null, object? Value = null)
{
    public static CommandResult Success(string? message = null, object? value = null) => new(true, message, value);

    public static CommandResult Failure(string? message = null, object? value = null) => new(false, message, value);
}

public interface ICommandDispatcher
{
    Task<CommandResult> DispatchAsync(IEditorCommand command, ActionContext context, CancellationToken cancellationToken = default);
}

public interface ITransactionScope : IAsyncDisposable
{
    string Description { get; }

    ValueTask CompleteAsync(CancellationToken cancellationToken = default);
}

public interface ITransactionScopeFactory
{
    ValueTask<ITransactionScope> BeginAsync(string description, ActionContext context, CancellationToken cancellationToken = default);
}
