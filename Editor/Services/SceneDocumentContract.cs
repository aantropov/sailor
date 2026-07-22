using SailorEditor.Content;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Services;

public sealed record SceneAssetTarget(
    string ScenePath,
    string AssetInfoPath,
    string Filename);

public enum SceneSaveOutcome
{
    Saved,
    Cancelled,
    Failed
}

public sealed record SceneSaveResult(
    SceneSaveOutcome Outcome,
    string? Path = null,
    string? Error = null)
{
    public bool Succeeded => Outcome == SceneSaveOutcome.Saved;
}

public static class SceneDocumentContract
{
    public const string WorldExtension = ".world";
    public const string WorldAssetInfoType = "Sailor::WorldPrefabAssetInfo";

    public static bool TryResolveSaveTarget(
        string activeContentRoot,
        string requestedPath,
        out SceneAssetTarget? target,
        out string error)
    {
        target = null;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(activeContentRoot) || string.IsNullOrWhiteSpace(requestedPath))
        {
            error = "A scene name is required.";
            return false;
        }

        try
        {
            var root = ProjectContentPathPolicy.NormalizeRoot(activeContentRoot);
            var candidate = requestedPath.Trim();
            if (string.IsNullOrEmpty(Path.GetExtension(candidate)))
                candidate += WorldExtension;
            if (!string.Equals(Path.GetExtension(candidate), WorldExtension, StringComparison.OrdinalIgnoreCase))
            {
                error = "Scene files must use the .world extension.";
                return false;
            }

            var scenePath = Path.GetFullPath(Path.IsPathRooted(candidate)
                ? candidate
                : Path.Combine(root, candidate));
            if (!ProjectContentPathPolicy.IsInsideRoot(root, scenePath))
            {
                error = "The scene must be saved inside the active Content root.";
                return false;
            }

            var filename = Path.GetFileName(scenePath);
            if (string.IsNullOrWhiteSpace(filename))
            {
                error = "A scene filename is required.";
                return false;
            }

            target = new SceneAssetTarget(scenePath, scenePath + ".asset", filename);
            return true;
        }
        catch (Exception exception)
        {
            error = exception.Message;
            return false;
        }
    }

    public static string BuildAssetInfo(string fileId, string filename)
    {
        if (string.IsNullOrWhiteSpace(fileId))
            throw new ArgumentException("A FileId is required.", nameof(fileId));
        if (string.IsNullOrWhiteSpace(filename))
            throw new ArgumentException("A scene filename is required.", nameof(filename));

        var root = new YamlMappingNode
        {
            { "assetInfoType", WorldAssetInfoType },
            { "fileId", fileId },
            { "filename", filename }
        };
        var yaml = new YamlStream(new YamlDocument(root));
        using var writer = new StringWriter();
        yaml.Save(writer, false);
        return writer.ToString();
    }
}
