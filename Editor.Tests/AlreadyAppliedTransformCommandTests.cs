using SailorEditor.Commands;
using SailorEditor.History;

namespace Editor.Tests;

public sealed class AlreadyAppliedTransformCommandTests
{
    [Fact]
    public async Task InitialExecuteProjectsOnly_WhileUndoAndRedoCommitToEngine()
    {
        var target = new RecordingTarget();
        var command = new AlreadyAppliedTransformCommand(
            "go-42",
            "before",
            "after",
            target,
            "Translate Duck");

        Assert.True(command.CanExecute(new ActionContext()));
        Assert.Null(command.MergePolicy);
        Assert.True((await command.ExecuteAsync(new ActionContext())).Succeeded);
        Assert.Equal(["local:go-42:after"], target.Calls);

        Assert.True((await command.UndoAsync(new ActionContext())).Succeeded);
        Assert.Equal(["local:go-42:after", "commit:go-42:before"], target.Calls);

        Assert.True((await command.ExecuteAsync(new ActionContext())).Succeeded);
        Assert.Equal(
            ["local:go-42:after", "commit:go-42:before", "commit:go-42:after"],
            target.Calls);
    }

    [Fact]
    public async Task InitialNativeStateIsAuthoritative_LocalProjectionFailureRefreshesAndEntersHistory()
    {
        var target = new RecordingTarget { LocalResult = false };
        var command = new AlreadyAppliedTransformCommand("go-42", "before", "after", target, "Scale Duck");

        Assert.True((await command.ExecuteAsync(new ActionContext())).Succeeded);
        Assert.Equal(["local:go-42:after", "refresh"], target.Calls);

        Assert.True((await command.UndoAsync(new ActionContext())).Succeeded);
        Assert.Equal(["local:go-42:after", "refresh", "commit:go-42:before"], target.Calls);
    }

    [Fact]
    public async Task NativeCommitSucceeded_LocalProjectionFailed_RefreshesAndReportsSuccess()
    {
        var calls = new List<string>();

        var result =
            await EditorViewportTransformApplication.CommitAndApplyAsync(
            _ =>
            {
                calls.Add("commit");
                return Task.FromResult(true);
            },
            () =>
            {
                calls.Add("local");
                return false;
            },
            _ =>
            {
                calls.Add("refresh");
                return Task.CompletedTask;
            });

        Assert.True(result);
        Assert.Equal(["commit", "local", "refresh"], calls);
    }

    [Fact]
    public async Task AlreadyAppliedState_LocalProjectionException_RefreshesAndReportsSuccess()
    {
        var calls = new List<string>();

        var result =
            await EditorViewportTransformApplication.ApplyAlreadyCommittedAsync(
            () =>
            {
                calls.Add("local");
                throw new InvalidOperationException("projection failed");
            },
            _ =>
            {
                calls.Add("refresh");
                return Task.CompletedTask;
            });

        Assert.True(result);
        Assert.Equal(["local", "refresh"], calls);
    }

    [Fact]
    public async Task NativeCommitFailed_DoesNotProjectOrRefresh()
    {
        var calls = new List<string>();

        var result =
            await EditorViewportTransformApplication.CommitAndApplyAsync(
            _ =>
            {
                calls.Add("commit");
                return Task.FromResult(false);
            },
            () =>
            {
                calls.Add("local");
                return true;
            },
            _ =>
            {
                calls.Add("refresh");
                return Task.CompletedTask;
            });

        Assert.False(result);
        Assert.Equal(["commit"], calls);
    }

    [Fact]
    public async Task CompletedGroup_ProjectsAllWithoutNativeCommit_AndUsesOneUndoRedoEntry()
    {
        var target = new RecordingTarget();
        var dispatcher = new EditorCommandDispatcher(new TestContextProvider(), new InMemoryUndoRedoHistory());
        var command = new AlreadyAppliedTransformGroupCommand("Rotate 2 objects", new[]
        {
            new AlreadyAppliedTransformCommand("A", "A-before", "A-after", target, "Rotate A"),
            new AlreadyAppliedTransformCommand("B", "B-before", "B-after", target, "Rotate B")
        }, _ => Task.FromResult(true));

        Assert.True((await dispatcher.DispatchAsync(command, new ActionContext())).Succeeded);
        Assert.Equal(new[] { "local:A:A-after", "local:B:B-after" }, target.Calls);
        Assert.Equal(1, dispatcher.UndoCount);
        Assert.Equal("Rotate 2 objects", dispatcher.UndoLabel);

        Assert.True(await dispatcher.UndoAsync());
        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(1, dispatcher.RedoCount);
        Assert.Equal(new[] { "commit:B:B-before", "commit:A:A-before" }, target.Calls.Skip(2));

        Assert.True(await dispatcher.RedoAsync());
        Assert.Equal(1, dispatcher.UndoCount);
        Assert.Equal(new[] { "commit:A:A-after", "commit:B:B-after" }, target.Calls.Skip(4));
    }

    [Fact]
    public async Task GroupUndo_FailedMemberRestoresEarlierMembersAndKeepsWholeHistoryEntry()
    {
        var target = new RecordingTarget();
        var dispatcher = new EditorCommandDispatcher(new TestContextProvider(), new InMemoryUndoRedoHistory());
        var command = new AlreadyAppliedTransformGroupCommand("Translate group", new[]
        {
            new AlreadyAppliedTransformCommand("A", "before", "after", target, "Translate A"),
            new AlreadyAppliedTransformCommand("B", "before", "after", target, "Translate B")
        }, _ => Task.FromResult(true));
        Assert.True((await dispatcher.DispatchAsync(command, new ActionContext())).Succeeded);
        target.RejectCommit = (id, yaml) => id == "A" && yaml == "before";

        Assert.False(await dispatcher.UndoAsync());
        Assert.Equal(1, dispatcher.UndoCount);
        Assert.Equal(0, dispatcher.RedoCount);
        Assert.Equal(new[] { "commit:B:before", "commit:A:before", "commit:B:after" }, target.Calls.Skip(2));

        Assert.Equal("after", target.Values["A"]);
        Assert.Equal("after", target.Values["B"]);
        target.RejectCommit = null;
        Assert.True(await dispatcher.UndoAsync());
        Assert.Equal(0, dispatcher.UndoCount);
        Assert.Equal(1, dispatcher.RedoCount);
        Assert.Equal("before", target.Values["A"]);
        Assert.Equal("before", target.Values["B"]);
    }

    [Fact]
    public async Task QueuedGroup_RevalidatesAfterEarlierCommandAndRejectsStaleProjection()
    {
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var revision = 1;
        var target = new RecordingTarget();
        var dispatcher = new EditorCommandDispatcher(new TestContextProvider(), new InMemoryUndoRedoHistory());
        var earlier = dispatcher.DispatchAsync(new BlockingCommand(async () =>
        {
            entered.SetResult();
            await release.Task;
            revision = 2;
        }), new ActionContext());
        await entered.Task;
        var group = new AlreadyAppliedTransformGroupCommand("Move group",
            [new AlreadyAppliedTransformCommand("A", "before", "after", target, "Move A")],
            _ => Task.FromResult(revision == 1));
        var queued = dispatcher.DispatchAsync(group, new ActionContext());
        Assert.False(queued.IsCompleted);
        release.SetResult();
        Assert.True((await earlier).Succeeded);
        Assert.False((await queued).Succeeded);
        Assert.Empty(target.Calls);
        Assert.Equal(0, dispatcher.UndoCount);
    }

    [Fact]
    public async Task NativeAppliedGroup_CancellationDuringProjectionFinishesAllMembersWithoutPartialUndo()
    {
        using var cancellation = new CancellationTokenSource();
        var target = new RecordingTarget { AfterLocalProjection = _ => cancellation.Cancel() };
        var group = new AlreadyAppliedTransformGroupCommand("Move group",
        [
            new AlreadyAppliedTransformCommand("A", "before", "after", target, "Move A"),
            new AlreadyAppliedTransformCommand("B", "before", "after", target, "Move B")
        ], _ => Task.FromResult(true));

        Assert.True((await group.ExecuteAsync(new ActionContext(), cancellation.Token)).Succeeded);
        Assert.True(cancellation.IsCancellationRequested);
        Assert.Equal(new[] { "local:A:after", "local:B:after" }, target.Calls);
        Assert.Equal("after", target.Values["A"]);
        Assert.Equal("after", target.Values["B"]);
    }

    sealed class BlockingCommand(Func<Task> execute) : IEditorCommand
    {
        public string Name => "Earlier managed mutation";
        public bool CanExecute(ActionContext context) => true;
        public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        {
            await execute();
            return CommandResult.Success();
        }
    }

    sealed class TestContextProvider : IActionContextProvider
    {
        public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null)
            => new(Origin: origin, Metadata: metadata);
    }

    sealed class RecordingTarget : IAlreadyAppliedTransformTarget
    {
        public List<string> Calls { get; } = [];
        public Dictionary<string, string> Values { get; } = [];
        public Action<string>? AfterLocalProjection { get; init; }
        public bool LocalResult { get; set; } = true;
        public bool CommitResult { get; set; } = true;
        public Func<string, string, bool>? RejectCommit { get; set; }

        public Task<bool> ApplyLocalAsync(
            string instanceId,
            string yaml,
            CancellationToken cancellationToken = default)
        {
            Calls.Add($"local:{instanceId}:{yaml}");
            Values[instanceId] = yaml;
            AfterLocalProjection?.Invoke(instanceId);
            return EditorViewportTransformApplication.ApplyAlreadyCommittedAsync(
                () => LocalResult,
                _ =>
                {
                    Calls.Add("refresh");
                    return Task.CompletedTask;
                },
                cancellationToken);
        }

        public Task<bool> CommitAndApplyLocalAsync(
            string instanceId,
            string yaml,
            CancellationToken cancellationToken = default)
        {
            Calls.Add($"commit:{instanceId}:{yaml}");
            var succeeded = CommitResult && !(RejectCommit?.Invoke(instanceId, yaml) ?? false);
            if (succeeded)
            {
                Values[instanceId] = yaml;
            }
            return Task.FromResult(succeeded);
        }
    }
}
