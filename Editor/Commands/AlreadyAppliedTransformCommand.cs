#nullable enable
using SailorEditor.History;

namespace SailorEditor.Commands;

public interface IAlreadyAppliedTransformTarget
{
    bool ApplyLocal(string instanceId, string yaml);
    bool CommitAndApplyLocal(string instanceId, string yaml);
}

internal static class EditorViewportTransformApplication
{
    public static bool ApplyAlreadyCommitted(
        Func<bool> applyLocal,
        Action refreshWorld)
    {
        try
        {
            if (applyLocal())
            {
                return true;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[EditorViewportTransform] Local projection failed after the native transform was applied: {ex}");
        }

        try
        {
            refreshWorld();
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[EditorViewportTransform] Failed to refresh the native world after a local projection failure: {ex}");
        }

        // The native transform is authoritative. Returning failure here would
        // either omit its history entry or leave history on the wrong side.
        return true;
    }

    public static bool CommitAndApply(
        Func<bool> commitEngine,
        Func<bool> applyLocal,
        Action refreshWorld)
    {
        if (!commitEngine())
        {
            return false;
        }

        return ApplyAlreadyCommitted(applyLocal, refreshWorld);
    }
}

public sealed class AlreadyAppliedTransformCommand : IUndoableEditorCommand
{
    readonly string _instanceId;
    readonly string _beforeYaml;
    readonly string _afterYaml;
    readonly IAlreadyAppliedTransformTarget _target;
    bool _initialExecutionPending = true;

    public AlreadyAppliedTransformCommand(
        string instanceId,
        string beforeYaml,
        string afterYaml,
        IAlreadyAppliedTransformTarget target,
        string description)
    {
        _instanceId = instanceId ?? string.Empty;
        _beforeYaml = beforeYaml ?? string.Empty;
        _afterYaml = afterYaml ?? string.Empty;
        _target = target ?? throw new ArgumentNullException(nameof(target));
        Description = description;
    }

    public string Name => nameof(AlreadyAppliedTransformCommand);
    public string Description { get; }
    public IHistoryMergePolicy? MergePolicy => null;

    public bool CanExecute(ActionContext context) =>
        !string.IsNullOrWhiteSpace(_instanceId) &&
        !string.IsNullOrWhiteSpace(_beforeYaml) &&
        !string.IsNullOrWhiteSpace(_afterYaml);

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_initialExecutionPending)
        {
            if (!_target.ApplyLocal(_instanceId, _afterYaml))
            {
                return Task.FromResult(CommandResult.Failure("The completed viewport transform could not be projected locally."));
            }

            _initialExecutionPending = false;
            return Task.FromResult(CommandResult.Success());
        }

        return Task.FromResult(ApplyCommitted(_afterYaml));
    }

    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_initialExecutionPending)
        {
            return ValueTask.FromResult(CommandResult.Failure("The viewport transform has not been applied yet."));
        }

        return ValueTask.FromResult(ApplyCommitted(_beforeYaml));
    }

    CommandResult ApplyCommitted(string yaml) =>
        _target.CommitAndApplyLocal(_instanceId, yaml)
            ? CommandResult.Success()
            : CommandResult.Failure("The viewport transform could not be committed to the engine.");
}
