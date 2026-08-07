#nullable enable

namespace SailorEditor.Mcp;

public sealed record McpAssetSnapshot(
    string FileId,
    string Name,
    string Type,
    string AssetInfoType,
    string SourcePath,
    string AssetInfoPath,
    bool IsReadOnly,
    bool IsMetadataLoaded,
    IReadOnlyDictionary<string, object?>? Properties,
    string? Yaml);

public sealed record McpAssetMutationResult(
    bool Succeeded,
    string Kind,
    string? FileId,
    string Message);
