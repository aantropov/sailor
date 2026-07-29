#nullable enable
using SailorEditor.History;

namespace SailorEditor.Commands;

public interface IAlreadyAppliedTransformTarget
{
    Task<bool> ApplyLocalAsync(
        string instanceId,
        string yaml,
        CancellationToken cancellationToken = default);
    Task<bool> CommitAndApplyLocalAsync(
        string instanceId,
        string yaml,
        CancellationToken cancellationToken = default);
}

internal static class EditorViewportTransformApplication
{
    public static async Task<bool> ApplyAlreadyCommittedAsync(
        Func<bool> applyLocal,
        Func<CancellationToken, Task> refreshWorld,
        CancellationToken cancellationToken = default)
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
            await refreshWorld(cancellationToken);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[EditorViewportTransform] Failed to refresh the native world after a local projection failure: {ex}");
        }

        // The native transform is authoritative. Returning failure here would
        // either omit its history entry or leave history on the wrong side.
        return true;
    }

    public static async Task<bool> CommitAndApplyAsync(
        Func<CancellationToken, Task<bool>> commitEngine,
        Func<bool> applyLocal,
        Func<CancellationToken, Task> refreshWorld,
        CancellationToken cancellationToken = default)
    {
        if (!await commitEngine(cancellationToken))
        {
            return false;
        }

        return await ApplyAlreadyCommittedAsync(
            applyLocal,
            refreshWorld,
            cancellationToken);
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

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_initialExecutionPending)
        {
            if (!await _target.ApplyLocalAsync(
                    _instanceId,
                    _afterYaml,
                    cancellationToken))
            {
                return CommandResult.Failure(
                    "The completed viewport transform could not be projected locally.");
            }

            _initialExecutionPending = false;
            return CommandResult.Success();
        }

        return await ApplyCommittedAsync(
            _afterYaml,
            cancellationToken);
    }

    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_initialExecutionPending)
        {
            return CommandResult.Failure(
                "The viewport transform has not been applied yet.");
        }

        return await ApplyCommittedAsync(
            _beforeYaml,
            cancellationToken);
    }

    async Task<CommandResult> ApplyCommittedAsync(
        string yaml,
        CancellationToken cancellationToken) =>
        await _target.CommitAndApplyLocalAsync(
            _instanceId,
            yaml,
            cancellationToken)
            ? CommandResult.Success()
            : CommandResult.Failure("The viewport transform could not be committed to the engine.");
}
