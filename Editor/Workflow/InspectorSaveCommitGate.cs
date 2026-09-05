using SailorEditor.ViewModels;

namespace SailorEditor.Workflow;

public static class InspectorSaveCommitGate
{
    public static async Task<bool> CommitSelectedAsync(
        object? selectedItem,
        CancellationToken cancellationToken = default)
    {
        if (selectedItem is not IInspectorEditable editable)
        {
            return true;
        }

        while (editable.HasPendingInspectorChanges)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var committed = await editable.CommitInspectorChangesAsync(
                cancellationToken);
            if (!editable.HasPendingInspectorChanges)
            {
                return true;
            }
            if (!committed && !editable.HasInFlightInspectorCommit)
            {
                return false;
            }

            await Task.Yield();
        }

        return true;
    }
}
