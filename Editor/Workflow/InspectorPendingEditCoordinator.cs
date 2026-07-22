#nullable enable

namespace SailorEditor.Workflow;

public sealed class InspectorPendingEditCoordinator
{
    public event Func<bool>? CommitPendingChangesRequested;

    public bool CommitPendingChanges()
    {
        if (CommitPendingChangesRequested is not { } requested)
        {
            return true;
        }

        foreach (Func<bool> handler in requested.GetInvocationList())
        {
            try
            {
                if (!handler())
                {
                    return false;
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector pending edit commit failed: {ex}");
                return false;
            }
        }

        return true;
    }
}
