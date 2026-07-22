namespace SailorEditor.Content;

public static class ProjectContentMutationReloadPolicy
{
    public static bool RequiresNativeReload(ProjectContentFileOperationResult operation)
        => operation.Succeeded || !operation.RollbackSucceeded;

    public static async Task<bool?> RequestNativeReloadIfRequiredAsync(
        ProjectContentFileOperationResult operation,
        Func<CancellationToken, Task<bool>> requestReload,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(requestReload);
        if (!RequiresNativeReload(operation))
            return null;

        return await requestReload(cancellationToken);
    }
}
