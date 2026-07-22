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
    public void NativeCommitSucceeded_LocalProjectionFailed_RefreshesAndReportsSuccess()
    {
        var calls = new List<string>();

        var result = EditorViewportTransformApplication.CommitAndApply(
            () =>
            {
                calls.Add("commit");
                return true;
            },
            () =>
            {
                calls.Add("local");
                return false;
            },
            () => calls.Add("refresh"));

        Assert.True(result);
        Assert.Equal(["commit", "local", "refresh"], calls);
    }

    [Fact]
    public void AlreadyAppliedState_LocalProjectionException_RefreshesAndReportsSuccess()
    {
        var calls = new List<string>();

        var result = EditorViewportTransformApplication.ApplyAlreadyCommitted(
            () =>
            {
                calls.Add("local");
                throw new InvalidOperationException("projection failed");
            },
            () => calls.Add("refresh"));

        Assert.True(result);
        Assert.Equal(["local", "refresh"], calls);
    }

    [Fact]
    public void NativeCommitFailed_DoesNotProjectOrRefresh()
    {
        var calls = new List<string>();

        var result = EditorViewportTransformApplication.CommitAndApply(
            () =>
            {
                calls.Add("commit");
                return false;
            },
            () =>
            {
                calls.Add("local");
                return true;
            },
            () => calls.Add("refresh"));

        Assert.False(result);
        Assert.Equal(["commit"], calls);
    }

    sealed class RecordingTarget : IAlreadyAppliedTransformTarget
    {
        public List<string> Calls { get; } = [];
        public bool LocalResult { get; set; } = true;
        public bool CommitResult { get; set; } = true;

        public bool ApplyLocal(string instanceId, string yaml)
        {
            Calls.Add($"local:{instanceId}:{yaml}");
            return EditorViewportTransformApplication.ApplyAlreadyCommitted(
                () => LocalResult,
                () => Calls.Add("refresh"));
        }

        public bool CommitAndApplyLocal(string instanceId, string yaml)
        {
            Calls.Add($"commit:{instanceId}:{yaml}");
            return CommitResult;
        }
    }
}
