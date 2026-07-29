using SailorEditor.Commands;
using SailorEditor.Protocol;

namespace Editor.Tests;

public sealed class HierarchyMutationRecoveryPolicyTests
{
    [Fact]
    public void StrictRestore_LostResponseWithExactProjection_ReconcilesSuccess()
    {
        var result = HierarchyMutationRecoveryPolicy.ResolveStrictRestore(
            EngineMutationResponseState.Unknown,
            AuthoritativeHierarchyProjection.Exact);

        Assert.True(result.Succeeded);
        Assert.False(result.RollbackOwnedHierarchy);
        Assert.True(result.OutcomeUncertain);
    }

    [Theory]
    [InlineData((int)AuthoritativeHierarchyProjection.Mismatch)]
    [InlineData((int)AuthoritativeHierarchyProjection.Unavailable)]
    public void StrictRestore_LostResponseWithoutOwnershipProof_NeverRollsBack(
        int projectionValue)
    {
        var result = HierarchyMutationRecoveryPolicy.ResolveStrictRestore(
            EngineMutationResponseState.Unknown,
            (AuthoritativeHierarchyProjection)projectionValue);

        Assert.False(result.Succeeded);
        Assert.False(result.RollbackOwnedHierarchy);
        Assert.True(result.OutcomeUncertain);
    }

    [Fact]
    public void StrictRestore_AcceptedResponse_AllowsExactRootRollback()
    {
        var result = HierarchyMutationRecoveryPolicy.ResolveStrictRestore(
            EngineMutationResponseState.Accepted,
            AuthoritativeHierarchyProjection.Mismatch);

        Assert.False(result.Succeeded);
        Assert.True(result.RollbackOwnedHierarchy);
    }

    [Fact]
    public void Destroy_LostResponseWithAbsentProjection_RetainsUndoHistory()
    {
        var result = HierarchyMutationRecoveryPolicy.ResolveDestroy(
            EngineMutationResponseState.Unknown,
            AuthoritativeHierarchyProjection.Absent);

        Assert.True(result.Succeeded);
        Assert.False(result.RetainRecoveryHistory);
    }

    [Fact]
    public void Destroy_LostResponseWithOriginalProjection_IsKnownNotApplied()
    {
        var result = HierarchyMutationRecoveryPolicy.ResolveDestroy(
            EngineMutationResponseState.Unknown,
            AuthoritativeHierarchyProjection.Exact);

        Assert.False(result.Succeeded);
        Assert.False(result.RetainRecoveryHistory);
    }

    [Theory]
    [InlineData((int)AuthoritativeHierarchyProjection.Mismatch)]
    [InlineData((int)AuthoritativeHierarchyProjection.Unavailable)]
    public void Destroy_AmbiguousProjection_RetainsRecoverableHistory(
        int projectionValue)
    {
        var result = HierarchyMutationRecoveryPolicy.ResolveDestroy(
            EngineMutationResponseState.Unknown,
            (AuthoritativeHierarchyProjection)projectionValue);

        Assert.True(result.Succeeded);
        Assert.True(result.RetainRecoveryHistory);
        Assert.True(result.OutcomeUncertain);
    }

    [Fact]
    public void Destroy_AmbiguousHistory_UndoAndRedoRemainRecoverable()
    {
        var ambiguousDelete =
            HierarchyMutationRecoveryPolicy.ResolveDestroy(
                EngineMutationResponseState.Unknown,
                AuthoritativeHierarchyProjection.Unavailable);
        var undo =
            HierarchyMutationRecoveryPolicy.ResolveDestroyUndo(
                ambiguousDelete.RetainRecoveryHistory,
                AuthoritativeHierarchyProjection.Exact);
        var redo =
            HierarchyMutationRecoveryPolicy.ResolveDestroy(
                EngineMutationResponseState.Unknown,
                AuthoritativeHierarchyProjection.Absent);

        Assert.True(ambiguousDelete.Succeeded);
        Assert.Equal(
            DestroyHierarchyUndoAction.CompleteWithoutMutation,
            undo);
        Assert.True(redo.Succeeded);
    }

    [Fact]
    public void Destroy_MismatchedConcurrentState_UndoNeverMutates()
    {
        var undo = HierarchyMutationRecoveryPolicy.ResolveDestroyUndo(
            recoveryHistoryRetained: true,
            AuthoritativeHierarchyProjection.Mismatch);

        Assert.Equal(
            DestroyHierarchyUndoAction.FailWithoutMutation,
            undo);
    }

    [Fact]
    public void LegacyInstantiateCall_WithThirdDefaultArgument_RemainsUnambiguous()
    {
        Func<EngineProtocolClient, Task<bool>> compileLegacyCall =
            client => client.InstantiatePrefabFromYamlAsync(
                "prefab: {}",
                string.Empty,
                default);

        Assert.NotNull(compileLegacyCall);
    }
}
