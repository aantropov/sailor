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

    private sealed class StubUndoableCommand(string description) : IUndoableEditorCommand
    {
        public string Name => description;
        public string Description => description;
        public IHistoryMergePolicy? MergePolicy => null;
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
            => Task.FromResult(CommandResult.Success());
        public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
            => ValueTask.CompletedTask;
    }
}
