#nullable enable

using System.Text.Json;
using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.Utility;

namespace SailorEditor.Mcp;

internal sealed class McpLandscapeOperations
{
    const string LandscapeComponentType = "Sailor::LandscapeComponent";

    readonly McpSceneBatchExecutor _sceneBatch;
    readonly ICommandHistoryService _history;
    readonly WorldService _world;
    readonly AssetsService _assets;

    public McpLandscapeOperations(
        McpSceneBatchExecutor sceneBatch,
        ICommandHistoryService history,
        WorldService world,
        AssetsService assets)
    {
        _sceneBatch = sceneBatch;
        _history = history;
        _world = world;
        _assets = assets;
    }

    public McpLandscapeVegetationSnapshot GetVegetation(
        string targetComponentId,
        long offset,
        int limit)
    {
        if (string.IsNullOrWhiteSpace(targetComponentId) ||
            !_world.TryGetComponent(
                new SailorEngine.InstanceId(targetComponentId.Trim()),
                out var component) ||
            !string.Equals(
                component.Typename?.Name,
                LandscapeComponentType,
                StringComparison.Ordinal))
        {
            return LandscapeVegetationBinary.Failure(
                string.Empty,
                string.Empty,
                "A valid LandscapeComponent instance ID is required.");
        }
        if (!component.OverrideProperties.TryGetValue(
                "vegetation",
                out var property) ||
            property is not Observable<SailorEngine.FileId> vegetation ||
            vegetation.Value is null ||
            vegetation.Value.IsEmpty())
        {
            return LandscapeVegetationBinary.Failure(
                string.Empty,
                string.Empty,
                "The LandscapeComponent does not reference a vegetation asset.");
        }

        var fileId = vegetation.Value.Value;
        if (!_assets.Assets.TryGetValue(vegetation.Value, out var asset) ||
            asset is not SailorEditor.ViewModels.LandscapeVegetationFile ||
            asset.Asset is null)
        {
            return LandscapeVegetationBinary.Failure(
                fileId,
                string.Empty,
                "The referenced vegetation asset is not available in the active project.");
        }
        return LandscapeVegetationBinary.ReadPage(
            asset.Asset.FullName,
            fileId,
            asset.Asset.FullName,
            offset,
            limit);
    }

    public Task<McpSceneBatchResult> ApplyAsync(
        McpLandscapeApplyRequest request,
        CancellationToken cancellationToken = default)
    {
        if (!McpLandscapeAuthoring.TryBuildOperations(request, out var operations, out var error))
        {
            return Task.FromResult(new McpSceneBatchResult(
                false,
                error,
                _history.WorkspaceEpoch,
                Array.Empty<McpSceneOperationResult>()));
        }

        return _sceneBatch.ExecuteAsync(
            new McpSceneBatchRequest(
                request.Confirm,
                request.ExpectedWorkspaceEpoch,
                string.IsNullOrWhiteSpace(request.TargetComponentId)
                    ? $"Create landscape '{request.Name}' through MCP"
                    : $"Author landscape '{request.Name}' through MCP",
                operations),
            cancellationToken);
    }

    public Task<McpSceneBatchResult> RegenerateAsync(
        bool confirm,
        long? expectedWorkspaceEpoch,
        string targetComponentId,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(targetComponentId))
        {
            return Task.FromResult(new McpSceneBatchResult(
                false,
                "A LandscapeComponent instance ID is required.",
                _history.WorkspaceEpoch,
                Array.Empty<McpSceneOperationResult>()));
        }

        return _sceneBatch.ExecuteAsync(
            new McpSceneBatchRequest(
                confirm,
                expectedWorkspaceEpoch,
                "Regenerate landscape through MCP",
                [
                    new McpSceneOperation
                    {
                        Kind = "update_component",
                        Target = targetComponentId,
                        Properties = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
                        {
                            ["regenerate"] = JsonSerializer.SerializeToElement(true),
                        },
                    },
                    new McpSceneOperation
                    {
                        Kind = "update_component",
                        Target = targetComponentId,
                        Properties = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
                        {
                            ["regenerate"] = JsonSerializer.SerializeToElement(false),
                        },
                    },
                ]),
            cancellationToken);
    }

}
