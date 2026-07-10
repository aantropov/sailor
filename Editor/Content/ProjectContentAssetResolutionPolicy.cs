namespace SailorEditor.Content;

public static class ProjectContentAssetResolutionPolicy
{
    public static bool ShouldReplace(bool existingIsReadOnly, bool incomingIsReadOnly)
        => existingIsReadOnly && !incomingIsReadOnly;
}
