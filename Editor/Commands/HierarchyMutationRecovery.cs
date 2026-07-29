namespace SailorEditor.Commands;

internal enum EngineMutationResponseState
{
    Accepted,
    Rejected,
    Unknown
}

internal enum AuthoritativeHierarchyProjection
{
    Unavailable,
    Absent,
    Exact,
    Mismatch
}

internal readonly record struct StrictHierarchyRestoreResolution(
    bool Succeeded,
    bool RollbackOwnedHierarchy,
    bool OutcomeUncertain);

internal readonly record struct DestroyHierarchyResolution(
    bool Succeeded,
    bool RetainRecoveryHistory,
    bool OutcomeUncertain);

internal enum DestroyHierarchyUndoAction
{
    Restore,
    CompleteWithoutMutation,
    FailWithoutMutation
}

internal sealed record HierarchyMutationRecoveryOutcome(
    string Operation,
    string RootInstanceId,
    AuthoritativeHierarchyProjection Projection,
    bool OutcomeUncertain);

internal static class HierarchyMutationRecoveryPolicy
{
    public static StrictHierarchyRestoreResolution ResolveStrictRestore(
        EngineMutationResponseState response,
        AuthoritativeHierarchyProjection projection)
    {
        if (projection == AuthoritativeHierarchyProjection.Exact)
        {
            return new StrictHierarchyRestoreResolution(
                Succeeded: true,
                RollbackOwnedHierarchy: false,
                OutcomeUncertain: response !=
                    EngineMutationResponseState.Accepted);
        }

        if (response == EngineMutationResponseState.Accepted)
        {
            return new StrictHierarchyRestoreResolution(
                Succeeded: false,
                RollbackOwnedHierarchy: true,
                OutcomeUncertain: projection ==
                    AuthoritativeHierarchyProjection.Unavailable);
        }

        return new StrictHierarchyRestoreResolution(
            Succeeded: false,
            RollbackOwnedHierarchy: false,
            OutcomeUncertain:
                projection == AuthoritativeHierarchyProjection.Mismatch ||
                projection == AuthoritativeHierarchyProjection.Unavailable);
    }

    public static DestroyHierarchyResolution ResolveDestroy(
        EngineMutationResponseState response,
        AuthoritativeHierarchyProjection projection)
    {
        if (response == EngineMutationResponseState.Accepted ||
            projection == AuthoritativeHierarchyProjection.Absent)
        {
            var uncertain =
                projection != AuthoritativeHierarchyProjection.Absent;
            return new DestroyHierarchyResolution(
                Succeeded: true,
                RetainRecoveryHistory: uncertain,
                OutcomeUncertain: uncertain);
        }

        if (projection == AuthoritativeHierarchyProjection.Exact)
        {
            return new DestroyHierarchyResolution(
                Succeeded: false,
                RetainRecoveryHistory: false,
                OutcomeUncertain: false);
        }

        return new DestroyHierarchyResolution(
            Succeeded: true,
            RetainRecoveryHistory: true,
            OutcomeUncertain: true);
    }

    public static DestroyHierarchyUndoAction ResolveDestroyUndo(
        bool recoveryHistoryRetained,
        AuthoritativeHierarchyProjection projection)
        => projection switch
        {
            AuthoritativeHierarchyProjection.Absent =>
                DestroyHierarchyUndoAction.Restore,
            AuthoritativeHierarchyProjection.Exact
                when recoveryHistoryRetained =>
                DestroyHierarchyUndoAction.CompleteWithoutMutation,
            _ => DestroyHierarchyUndoAction.FailWithoutMutation
        };
}
