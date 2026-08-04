using SailorEditor.Services;

public class AssetCacheIndexStoreTests
{
    [Fact]
    public void Parse_LoadsStrictV1IndexWithoutReadingAssetSidecars()
    {
        var sourcePath = Path.GetFullPath(Path.Combine(
            Path.GetTempPath(),
            "Sailor Asset Index",
            "Duck.glb"));
        var result = AssetCacheIndexStore.Parse(
            "AssetCache.yaml",
            CreateCacheYaml(sourcePath),
            "workspace-test");

        Assert.Equal(AssetCacheIndexStatus.Loaded, result.Status);
        var entry = Assert.Single(result.Entries!);
        Assert.Equal("{ASSET-ID}", entry.FileId);
        Assert.Equal(sourcePath, entry.SourcePath);
        Assert.Equal("Duck.glb.asset", entry.MetadataFilename);
        Assert.Equal("Sailor::ModelAssetInfo", entry.AssetInfoType);
    }

    [Fact]
    public void Parse_RejectsOlderOrNewerCacheVersions()
    {
        var yaml = CreateCacheYaml(Path.GetFullPath("Duck.glb"))
            .Replace("cacheVersion: 1", "cacheVersion: 2", StringComparison.Ordinal);

        var result = AssetCacheIndexStore.Parse(
            "AssetCache.yaml",
            yaml,
            "workspace-test");

        Assert.Equal(AssetCacheIndexStatus.UnsupportedVersion, result.Status);
        Assert.Null(result.Entries);
    }

    [Fact]
    public void Parse_RejectsUnknownLegacyEntryFields()
    {
        var yaml = CreateCacheYaml(
            Path.GetFullPath("Duck.glb"),
            "metadataPath: Duck.glb.asset");

        var result = AssetCacheIndexStore.Parse(
            "AssetCache.yaml",
            yaml,
            "workspace-test");

        Assert.Equal(AssetCacheIndexStatus.Corrupt, result.Status);
        Assert.Contains("metadataPath", result.Diagnostic, StringComparison.Ordinal);
    }

    static string CreateCacheYaml(
        string sourcePath,
        string? extraEntryField = null)
    {
        var escapedSourcePath = sourcePath
            .Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal);
        var payload = $$"""
            assetCache:
              assets:
                "{ASSET-ID}":
                  fileId: "{ASSET-ID}"
                  assetImportTime: 1
                  sourcePath: "{{escapedSourcePath}}"
                  sourceRevision:
                    modificationTimeNanoseconds: 2
                    fileSize: 3
                    contentHash: 0
                  metadataFilename: Duck.glb.asset
                  metadataRevision:
                    modificationTimeNanoseconds: 4
                    fileSize: 5
                    contentHash: 0
                  assetInfoType: Sailor::ModelAssetInfo
            """;
        if (!string.IsNullOrWhiteSpace(extraEntryField))
        {
            payload = payload.Replace(
                "      assetInfoType: Sailor::ModelAssetInfo",
                $"      assetInfoType: Sailor::ModelAssetInfo{Environment.NewLine}      {extraEntryField}",
                StringComparison.Ordinal);
        }

        var escapedPayload = payload
            .Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\r", "", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal);

        return $$"""
            cacheVersion: 1
            payloadVersion: 1
            cacheKind: asset-cache
            workspaceId: workspace-test
            engineVersion: 0.1
            buildIdentity: test
            producerIdentity: asset-cache-v1
            payload: "{{escapedPayload}}"
            """;
    }
}
