using SailorEditor.Commands;

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

    sealed class RecordingTarget : IAlreadyAppliedTransformTarget
    {
        public List<string> Calls { get; } = [];
        public bool LocalResult { get; set; } = true;
        public bool CommitResult { get; set; } = true;

        public Task<bool> ApplyLocalAsync(
            string instanceId,
            string yaml,
            CancellationToken cancellationToken = default)
        {
            Calls.Add($"local:{instanceId}:{yaml}");
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
            return Task.FromResult(CommitResult);
        }
    }
}
