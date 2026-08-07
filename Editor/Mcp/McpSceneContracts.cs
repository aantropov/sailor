#nullable enable

using System.Text.Json;

namespace SailorEditor.Mcp;

public sealed record McpSceneOperation
{
    public string Kind { get; init; } = string.Empty;
    public string? Alias { get; init; }
    public string? Target { get; init; }
    public string? Parent { get; init; }
    public string? Name { get; init; }
    public string? ComponentType { get; init; }
    public bool KeepWorldTransform { get; init; } = true;
    public Dictionary<string, JsonElement> Properties { get; init; } =
        new(StringComparer.Ordinal);
}

public sealed record McpSceneBatchRequest(
    bool Confirm,
    long? ExpectedWorkspaceEpoch,
    string? Description,
    IReadOnlyList<McpSceneOperation> Operations);

public sealed record McpSceneOperationResult(
    int Index,
    string Kind,
    bool Succeeded,
    string? InstanceId,
    string? Alias,
    string? Message);

public sealed record McpSceneBatchResult(
    bool Succeeded,
    string Message,
    long WorkspaceEpoch,
    IReadOnlyList<McpSceneOperationResult> Operations);

public sealed record McpComponentSnapshot(
    string InstanceId,
    string Type,
    IReadOnlyDictionary<string, object?> Properties,
    string? Yaml);

public sealed record McpGameObjectSnapshot(
    string InstanceId,
    string Name,
    string? ParentInstanceId,
    string? PrefabFileId,
    IReadOnlyDictionary<string, object?> Properties,
    IReadOnlyList<McpComponentSnapshot> Components,
    IReadOnlyList<McpGameObjectSnapshot> Children);

public sealed record McpSceneHierarchySnapshot(
    string Name,
    string? WorldFileId,
    long WorkspaceEpoch,
    IReadOnlyList<McpGameObjectSnapshot> Roots,
    string? Yaml);

public sealed record McpComponentPropertySchema(
    string Name,
    string Type,
    bool ReadOnly,
    object? DefaultValue,
    IReadOnlyList<string>? AllowedValues,
    string? ObjectType);

public sealed record McpComponentTypeSchema(
    string Name,
    string Base,
    IReadOnlyList<McpComponentPropertySchema> Properties);
