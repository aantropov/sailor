using SailorEditor.Content;

namespace Editor.Tests;

public sealed class ProjectContentMutationReloadPolicyTests
{
    [Fact]
    public async Task SuccessfulMutation_RequestsExactlyOneNativeReload()
    {
        var calls = 0;
        var operation = new ProjectContentFileOperationResult(true, null, true, Array.Empty<string>());

        var accepted = await ProjectContentMutationReloadPolicy.RequestNativeReloadIfRequiredAsync(
            operation,
            _ =>
            {
                calls++;
                return Task.FromResult(true);
            });

        Assert.True(accepted);
        Assert.Equal(1, calls);
    }

    [Fact]
    public async Task FailedMutationWithCompleteRollback_DoesNotRequestNativeReload()
    {
        var calls = 0;
        var operation = new ProjectContentFileOperationResult(false, "failed", true, Array.Empty<string>());

        var accepted = await ProjectContentMutationReloadPolicy.RequestNativeReloadIfRequiredAsync(
            operation,
            _ =>
            {
                calls++;
                return Task.FromResult(true);
            });

        Assert.Null(accepted);
        Assert.Equal(0, calls);
    }

    [Fact]
    public async Task FailedMutationWithIncompleteRollback_RequestsExactlyOneReconciliationReload()
    {
        var calls = 0;
        var operation = new ProjectContentFileOperationResult(
            false,
            "failed",
            false,
            new[] { "/Content/Models/Duck.glb" });

        var accepted = await ProjectContentMutationReloadPolicy.RequestNativeReloadIfRequiredAsync(
            operation,
            _ =>
            {
                calls++;
                return Task.FromResult(false);
            });

        Assert.False(accepted);
        Assert.Equal(1, calls);
    }
}
