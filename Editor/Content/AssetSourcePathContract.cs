namespace SailorEditor.Content;

public readonly record struct AssetSourcePathResolution(
    string SourcePath,
    bool OwnsSourceFile,
    string AssetExtension);

public static class AssetSourcePathContract
{
    public static bool TryResolve(
        string metadataPath,
        string? declaredFilename,
        out AssetSourcePathResolution resolution,
        out string error)
    {
        resolution = default;
        error = string.Empty;
        if (string.IsNullOrWhiteSpace(metadataPath) || string.IsNullOrWhiteSpace(declaredFilename))
        {
            error = "Asset metadata must declare a source filename.";
            return false;
        }

        if (Path.IsPathRooted(declaredFilename) ||
            declaredFilename is "." or ".." ||
            declaredFilename.Contains('/') ||
            declaredFilename.Contains('\\') ||
            !string.Equals(Path.GetFileName(declaredFilename), declaredFilename, StringComparison.Ordinal))
        {
            error = "Asset metadata filename must be a basename in the metadata directory.";
            return false;
        }

        var canonicalMetadataPath = Path.GetFullPath(metadataPath);
        var metadataDirectory = Path.GetDirectoryName(canonicalMetadataPath);
        if (string.IsNullOrWhiteSpace(metadataDirectory))
        {
            error = "Asset metadata path has no parent directory.";
            return false;
        }

        var sourcePath = Path.GetFullPath(Path.Combine(metadataDirectory, declaredFilename));
        if (!ProjectContentPathPolicy.IsSamePath(Path.GetDirectoryName(sourcePath)!, metadataDirectory))
        {
            error = "Asset metadata source must stay in the metadata directory.";
            return false;
        }
        if (!File.Exists(sourcePath))
        {
            error = $"Asset metadata source does not exist: {sourcePath}";
            return false;
        }

        var ownsSourceFile = ProjectContentPathPolicy.IsSamePath(
            canonicalMetadataPath,
            sourcePath + ".asset");
        var typedAssetPath = ownsSourceFile
            ? sourcePath
            : Path.ChangeExtension(canonicalMetadataPath, null);
        resolution = new AssetSourcePathResolution(
            sourcePath,
            ownsSourceFile,
            Path.GetExtension(typedAssetPath));
        return true;
    }
}
