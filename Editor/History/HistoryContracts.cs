using SailorEditor.Commands;

namespace SailorEditor.History;

public sealed record HistoryEntry(
    string Description,
    IUndoableEditorCommand Command,
    CommandOrigin? Origin = null,
    DateTimeOffset? Timestamp = null,
    IReadOnlyDictionary<string, string?>? Metadata = null)
{
    public DateTimeOffset EffectiveTimestamp => Timestamp ?? DateTimeOffset.UtcNow;
}

public interface IHistoryMergePolicy
{
    bool CanMerge(HistoryEntry previous, HistoryEntry next);

    HistoryEntry Merge(HistoryEntry previous, HistoryEntry next);
}

public interface IUndoRedoHistory
{
    int UndoCount { get; }

    int RedoCount { get; }

    HistoryEntry? PeekUndo();

    HistoryEntry? PeekRedo();

    void Push(HistoryEntry entry);

    HistoryEntry PopUndo();

    HistoryEntry PopRedo();

    void PushRedo(HistoryEntry entry);

    void Clear();
}

public sealed class InMemoryUndoRedoHistory : IUndoRedoHistory
{
    private readonly Stack<HistoryEntry> _undo = new();
    private readonly Stack<HistoryEntry> _redo = new();

    public int UndoCount => _undo.Count;

    public int RedoCount => _redo.Count;

    public HistoryEntry? PeekUndo() => _undo.Count > 0 ? _undo.Peek() : null;

    public HistoryEntry? PeekRedo() => _redo.Count > 0 ? _redo.Peek() : null;

    public void Push(HistoryEntry entry)
    {
        _undo.Push(entry);
        _redo.Clear();
    }

    public HistoryEntry PopUndo() => _undo.Pop();

    public HistoryEntry PopRedo() => _redo.Pop();

    public void PushRedo(HistoryEntry entry) => _redo.Push(entry);

    public void Clear()
    {
        _undo.Clear();
        _redo.Clear();
    }
}
