using SailorEditor.Commands;
using SailorEditor.History;

namespace Editor.Tests;

public sealed class EditorCommandBatchTests
{
    [Fact]
    public async Task UndoableBatch_UsesOneHistoryEntryAndReversesEveryItem()
    {
        var events = new List<string>();
        var dispatcher = new EditorCommandDispatcher(new TestContextProvider(), new InMemoryUndoRedoHistory());
        var command = EditorCommandBatch.Create("Delete selection", new[]
        {
            new UndoableCommand("A", events),
            new UndoableCommand("B", events)
        });

        var result = await dispatcher.DispatchAsync(command, new ActionContext());
        Assert.True(result.Succeeded);
        Assert.Equal(new[] { "A", "B" }, EditorCommandBatch.ResultValues<string>(result));
        Assert.Equal(1, dispatcher.UndoCount);
        Assert.True(await dispatcher.UndoAsync());
        Assert.True(await dispatcher.RedoAsync());
        Assert.Equal(new[] { "+A", "+B", "-B", "-A", "+A", "+B" }, events);
    }

    [Fact]
    public async Task UndoableBatch_FailedSecondItemRollsBackFirst()
    {
        var events = new List<string>();
        var dispatcher = new EditorCommandDispatcher(new TestContextProvider(), new InMemoryUndoRedoHistory());
        var command = EditorCommandBatch.Create("Reparent selection", new[]
        {
            new UndoableCommand("A", events),
            new UndoableCommand("B", events, succeeds: false)
        });

        Assert.False((await dispatcher.DispatchAsync(command, new ActionContext())).Succeeded);
        Assert.Equal(new[] { "+A", "+B", "-A" }, events);
        Assert.Equal(0, dispatcher.UndoCount);
    }

    [Fact]
    public async Task AssetBatch_PreflightsEntireSelectionBeforeFirstMutation()
    {
        var events = new List<string>();
        var command = EditorCommandBatch.Create("Move selection", new[]
        {
            new PlainCommand("A", events),
            new PlainCommand("B", events, canExecute: false)
        });

        Assert.False(command.CanExecute(new ActionContext()));
        Assert.False((await command.ExecuteAsync(new ActionContext())).Succeeded);
        Assert.Empty(events);
        Assert.IsNotAssignableFrom<IUndoableEditorCommand>(command);
    }

    [Fact]
    public async Task AssetBatch_FailedItemReportsProgressAndDoesNotRunLaterItems()
    {
        var events = new List<string>();
        var command = EditorCommandBatch.Create("Move selection", new[]
        {
            new PlainCommand("A", events),
            new PlainCommand("B", events, succeeds: false),
            new PlainCommand("C", events)
        });

        var result = await command.ExecuteAsync(new ActionContext());

        Assert.False(result.Succeeded);
        Assert.Contains("completed 1 of 3", result.Message);
        Assert.Contains("B", result.Message);
        Assert.Equal(new[] { "A", "B" }, events);
        var completed = Assert.IsAssignableFrom<IReadOnlyList<CommandResult>>(result.Value);
        Assert.Equal(2, completed.Count);
        Assert.True(completed[0].Succeeded);
        Assert.False(completed[1].Succeeded);
    }

    [Fact]
    public void Create_SingleItemPreservesItsExistingCommandContract()
    {
        var item = new PlainCommand("A", []);
        Assert.Same(item, EditorCommandBatch.Create("A", new[] { item }));
        Assert.Throws<ArgumentException>(() => EditorCommandBatch.Create("Empty", Array.Empty<IEditorCommand>()));
    }

    sealed class TestContextProvider : IActionContextProvider
    {
        public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null)
            => new(Origin: origin, Metadata: metadata);
    }

    sealed class UndoableCommand(string name, List<string> events, bool succeeds = true) : IUndoableEditorCommand
    {
        public string Name => name;
        public string Description => name;
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            events.Add("+" + name);
            return Task.FromResult(succeeds ? CommandResult.Success(value: name) : CommandResult.Failure("Rejected"));
        }
        public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            events.Add("-" + name);
            return ValueTask.FromResult(CommandResult.Success());
        }
    }

    sealed class PlainCommand(string name, List<string> events, bool canExecute = true, bool succeeds = true) : IEditorCommand
    {
        public string Name => name;
        public bool CanExecute(ActionContext context) => canExecute;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            events.Add(name);
            return Task.FromResult(succeeds ? CommandResult.Success() : CommandResult.Failure("Rejected"));
        }
    }
}
