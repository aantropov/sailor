namespace SailorEditor.ViewModels;

public interface IInspectorEditable
{
    bool HasPendingInspectorChanges { get; }
    bool HasInFlightInspectorCommit { get; }
    Task<bool> CommitInspectorChangesAsync(
        CancellationToken cancellationToken = default);
}
