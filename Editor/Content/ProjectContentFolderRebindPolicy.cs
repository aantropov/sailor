namespace SailorEditor.Content;

public enum ProjectContentFolderMutationKind
{
    MoveOrRename,
    Delete
}

public readonly record struct ProjectContentFolderRebindPlan(
    string? OriginalPath,
    string? SuccessPath)
{
    public bool HasSelection => !string.IsNullOrWhiteSpace(OriginalPath);
}

public static class ProjectContentFolderRebindPolicy
{
    public static ProjectContentFolderRebindPlan CreatePlan(
        string? currentFolderPath,
        string mutatedFolderPath,
        string? targetFolderPath,
        ProjectContentFolderMutationKind mutationKind)
    {
        if (string.IsNullOrWhiteSpace(currentFolderPath))
            return default;

        var originalPath = ProjectContentPathPolicy.NormalizeRoot(currentFolderPath);
        var sourcePath = ProjectContentPathPolicy.NormalizeRoot(mutatedFolderPath);
        if (!ProjectContentPathPolicy.IsInsideRoot(sourcePath, originalPath))
            return new(originalPath, originalPath);

        if (mutationKind == ProjectContentFolderMutationKind.Delete)
            return new(originalPath, Path.GetDirectoryName(sourcePath));

        if (string.IsNullOrWhiteSpace(targetFolderPath))
            throw new ArgumentException("A move or rename requires a target folder path.", nameof(targetFolderPath));

        var targetPath = ProjectContentPathPolicy.NormalizeRoot(targetFolderPath);
        var relativePath = Path.GetRelativePath(sourcePath, originalPath);
        var mappedPath = relativePath == "."
            ? targetPath
            : ProjectContentPathPolicy.NormalizeRoot(Path.Combine(targetPath, relativePath));
        return new(originalPath, mappedPath);
    }

    public static int? ResolveLiveFolderId(
        string? preferredPath,
        IReadOnlyList<ProjectContentFolderSnapshot> liveFolders)
    {
        if (string.IsNullOrWhiteSpace(preferredPath))
            return null;

        string? candidate;
        try
        {
            candidate = ProjectContentPathPolicy.NormalizeRoot(preferredPath);
        }
        catch
        {
            return null;
        }

        while (!string.IsNullOrWhiteSpace(candidate))
        {
            var match = liveFolders.FirstOrDefault(folder =>
                !string.IsNullOrWhiteSpace(folder.FullPath) &&
                ProjectContentPathPolicy.IsSamePath(folder.FullPath, candidate));
            if (match is not null)
                return match.Id;

            var parent = Path.GetDirectoryName(candidate);
            if (string.IsNullOrWhiteSpace(parent) || ProjectContentPathPolicy.IsSamePath(parent, candidate))
                break;

            candidate = parent;
        }

        return null;
    }
}
