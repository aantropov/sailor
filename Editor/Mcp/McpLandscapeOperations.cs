#nullable enable

using System.Text.Json;
using SailorEditor.Commands;

namespace SailorEditor.Mcp;

internal sealed class McpLandscapeOperations
{
    const string LandscapeComponentType = "Sailor::LandscapeComponent";

    readonly McpSceneBatchExecutor _sceneBatch;
    readonly ICommandHistoryService _history;

    public McpLandscapeOperations(
        McpSceneBatchExecutor sceneBatch,
        ICommandHistoryService history)
    {
        _sceneBatch = sceneBatch;
        _history = history;
    }

    public Task<McpSceneBatchResult> ApplyAsync(
        McpLandscapeApplyRequest request,
        CancellationToken cancellationToken = default)
    {
        if (!TryBuildOperations(request, out var operations, out var error))
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

    internal static bool TryBuildOperations(
        McpLandscapeApplyRequest request,
        out IReadOnlyList<McpSceneOperation> operations,
        out string error)
    {
        operations = Array.Empty<McpSceneOperation>();
        error = string.Empty;

        if (request.ChunksX is < 1 or > 64 || request.ChunksZ is < 1 or > 64)
            return Fail("chunksX and chunksZ must be between 1 and 64.", out error);
        if (!IsFinitePositive(request.ChunkSize))
            return Fail("chunkSize must be a finite positive number.", out error);
        if (request.ChunkResolution is < 2 or > 128)
            return Fail("chunkResolution must be between 2 and 128.", out error);
        if (!float.IsFinite(request.HeightScale) || request.HeightScale < 0.0f)
            return Fail("heightScale must be a finite non-negative number.", out error);
        if (!IsFinitePositive(request.NoiseScale))
            return Fail("noiseScale must be a finite positive number.", out error);
        if (!IsFinitePositive(request.TextureTiling))
            return Fail("textureTiling must be a finite positive number.", out error);
        if (request.LodDistances is null || request.LodDistances.Length > 7 ||
            request.LodDistances.Any(value => !IsFinitePositive(value)) ||
            !request.LodDistances.SequenceEqual(request.LodDistances.Order()))
        {
            return Fail("lodDistances must contain at most seven finite positive values in ascending order.", out error);
        }
        if (!float.IsFinite(request.LodSkirtDepth) || request.LodSkirtDepth is < 0.0f or > 64.0f)
            return Fail("lodSkirtDepth must be finite and between 0 and 64.", out error);
        if (request.GrassInstanceBudget > 1048576)
            return Fail("grassInstanceBudget cannot exceed 1048576.", out error);
        if (!float.IsFinite(request.GrassResidencyHysteresis) ||
            request.GrassResidencyHysteresis is < 0.0f or > 512.0f)
        {
            return Fail("grassResidencyHysteresis must be finite and between 0 and 512.", out error);
        }
        if (string.IsNullOrWhiteSpace(request.MaterialFileId))
            return Fail("materialFileId is required.", out error);
        if (request.LayerTextureFileIds is null || request.LayerTextureFileIds.Length != 4 ||
            request.LayerTextureFileIds.Any(string.IsNullOrWhiteSpace))
        {
            return Fail("Exactly four non-empty layerTextureFileIds are required.", out error);
        }
        if (request.MaterialMaskFileIds is null || request.MaterialMaskFileIds.Length > 4 ||
            request.MaterialMaskFileIds.Any(string.IsNullOrWhiteSpace))
        {
            return Fail("materialMaskFileIds must contain zero to four non-empty texture IDs.", out error);
        }

        var vegetation = request.Vegetation ?? [];
        var shadowModes = new List<float>(vegetation.Length);
        var residencyModes = new List<float>(vegetation.Length);
        foreach (var profile in vegetation)
        {
            if (string.IsNullOrWhiteSpace(profile.ModelFileId))
            {
                return Fail("Every vegetation profile requires modelFileId; an empty materialFileId uses the GLB materials.", out error);
            }
            if (profile.InstancesPerChunk > 2048)
                return Fail("vegetation instancesPerChunk cannot exceed 2048.", out error);
            if (profile.MeshIndex < -1)
                return Fail("vegetation meshIndex must be -1 (all meshes) or a non-negative mesh index.", out error);
            if (!TryParseResidency(profile.Residency, out var residencyMode))
                return Fail("vegetation residency must be Persistent or Grass.", out error);
            if (!float.IsFinite(profile.Priority) || profile.Priority is < 0.0f or > 100.0f)
                return Fail("vegetation priority must be finite and between 0 and 100.", out error);
            if (!IsFinitePositive(profile.MinScale) ||
                !IsFinitePositive(profile.MaxScale) ||
                profile.MaxScale < profile.MinScale)
            {
                return Fail("Every vegetation profile requires 0 < minScale <= maxScale.", out error);
            }
            if (!float.IsFinite(profile.GroundOffset))
                return Fail("vegetation groundOffset must be finite.", out error);
            if (!TryParseShadowMode(profile.Shadows, out var shadowMode))
                return Fail("vegetation shadows must be None, NearOnly, or All.", out error);
            if (shadowMode == 1.0f && !IsFinitePositive(profile.ShadowDistance))
                return Fail("NearOnly vegetation requires a positive shadowDistance.", out error);
            if (profile.MinLod > 15 || profile.MaxLod > 15 || profile.MaxLod < profile.MinLod)
                return Fail("vegetation LOD range must satisfy 0 <= minLod <= maxLod <= 15.", out error);
            if (!IsCoverage(profile.Lod1ScreenCoverage) || !IsCoverage(profile.Lod2ScreenCoverage) ||
                profile.Lod1ScreenCoverage < profile.Lod2ScreenCoverage)
            {
                return Fail("vegetation LOD screen coverage must be in 0..1 and descend from LOD1 to LOD2.", out error);
            }
            if (!IsFinitePositive(profile.CullDistance))
                return Fail("vegetation cullDistance must be a finite positive number.", out error);
            if (!float.IsFinite(profile.ColliderRadius) || profile.ColliderRadius < 0.0f ||
                !float.IsFinite(profile.ColliderHeight) || profile.ColliderHeight < profile.ColliderRadius * 2.0f ||
                !float.IsFinite(profile.ColliderOffsetY))
            {
                return Fail("vegetation collider requires radius >= 0, height >= 2 * radius, and a finite Y offset.", out error);
            }
            if (residencyMode == 1.0f && profile.ColliderRadius > 0.0f)
                return Fail("Grass vegetation cannot create colliders; use Persistent residency for collidable profiles.", out error);
            shadowModes.Add(shadowMode);
            residencyModes.Add(residencyMode);
        }

        var sculptValues = new List<float>((request.Sculpt?.Length ?? 0) * 5);
        foreach (var stamp in request.Sculpt ?? [])
        {
            if (!AllFinite(stamp.X, stamp.Z, stamp.Radius, stamp.Strength) ||
                stamp.Radius <= 0.0f || stamp.Strength < 0.0f)
            {
                return Fail("Every sculpt stamp requires finite coordinates, radius > 0 and strength >= 0.", out error);
            }
            if (!TryParseSculptOperation(stamp.Operation, out var operation))
                return Fail("sculpt operation must be Raise, Lower, or Smooth.", out error);
            sculptValues.AddRange([stamp.X, stamp.Z, stamp.Radius, stamp.Strength, operation]);
        }

        var paintValues = new List<float>((request.Paint?.Length ?? 0) * 5);
        foreach (var stamp in request.Paint ?? [])
        {
            if (!AllFinite(stamp.X, stamp.Z, stamp.Radius, stamp.Strength) ||
                stamp.Radius <= 0.0f || stamp.Strength < 0.0f || stamp.Layer > 3)
            {
                return Fail("Every paint stamp requires finite coordinates, radius > 0, strength >= 0 and layer 0..3.", out error);
            }
            paintValues.AddRange([stamp.X, stamp.Z, stamp.Radius, stamp.Strength, stamp.Layer]);
        }

        var properties = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
        {
            ["chunksX"] = JsonSerializer.SerializeToElement(request.ChunksX),
            ["chunksZ"] = JsonSerializer.SerializeToElement(request.ChunksZ),
            ["chunkSize"] = JsonSerializer.SerializeToElement(request.ChunkSize),
            ["chunkResolution"] = JsonSerializer.SerializeToElement(request.ChunkResolution),
            ["heightScale"] = JsonSerializer.SerializeToElement(request.HeightScale),
            ["noiseScale"] = JsonSerializer.SerializeToElement(request.NoiseScale),
            ["seed"] = JsonSerializer.SerializeToElement(request.Seed),
            ["material"] = JsonSerializer.SerializeToElement(new { fileId = request.MaterialFileId, instanceId = string.Empty }),
            ["layerTextures"] = JsonSerializer.SerializeToElement(request.LayerTextureFileIds),
            ["heightmapTexture"] = JsonSerializer.SerializeToElement(request.HeightmapTextureFileId ?? string.Empty),
            ["materialMasks"] = JsonSerializer.SerializeToElement(request.MaterialMaskFileIds),
            ["textureTiling"] = JsonSerializer.SerializeToElement(request.TextureTiling),
            ["lodDistances"] = JsonSerializer.SerializeToElement(request.LodDistances),
            ["lodSkirtDepth"] = JsonSerializer.SerializeToElement(request.LodSkirtDepth),
            ["grassInstanceBudget"] = JsonSerializer.SerializeToElement(request.GrassInstanceBudget),
            ["grassResidencyHysteresis"] = JsonSerializer.SerializeToElement(request.GrassResidencyHysteresis),
            ["sculptStamps"] = JsonSerializer.SerializeToElement(sculptValues),
            ["paintStamps"] = JsonSerializer.SerializeToElement(paintValues),
            ["vegetationModels"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.ModelFileId)),
            ["vegetationMaterials"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.MaterialFileId)),
            ["vegetationMeshIndex"] = JsonSerializer.SerializeToElement(vegetation.Select(value => (float)value.MeshIndex)),
            ["vegetationInstancesPerChunk"] = JsonSerializer.SerializeToElement(vegetation.Select(value => (float)value.InstancesPerChunk)),
            ["vegetationResidency"] = JsonSerializer.SerializeToElement(residencyModes),
            ["vegetationPriority"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.Priority)),
            ["vegetationMinScale"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.MinScale)),
            ["vegetationMaxScale"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.MaxScale)),
            ["vegetationGroundOffset"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.GroundOffset)),
            ["vegetationShadowMode"] = JsonSerializer.SerializeToElement(shadowModes),
            ["vegetationShadowDistance"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.ShadowDistance)),
            ["vegetationMinLod"] = JsonSerializer.SerializeToElement(vegetation.Select(value => (float)value.MinLod)),
            ["vegetationMaxLod"] = JsonSerializer.SerializeToElement(vegetation.Select(value => (float)value.MaxLod)),
            ["vegetationLod1ScreenCoverage"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.Lod1ScreenCoverage)),
            ["vegetationLod2ScreenCoverage"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.Lod2ScreenCoverage)),
            ["vegetationCullDistance"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.CullDistance)),
            ["vegetationColliderRadius"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.ColliderRadius)),
            ["vegetationColliderHeight"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.ColliderHeight)),
            ["vegetationColliderOffsetY"] = JsonSerializer.SerializeToElement(vegetation.Select(value => value.ColliderOffsetY)),
            ["regenerate"] = JsonSerializer.SerializeToElement(false),
            ["flatten"] = JsonSerializer.SerializeToElement(false),
        };

        if (string.IsNullOrWhiteSpace(request.TargetComponentId))
        {
            operations =
            [
                new McpSceneOperation
                {
                    Kind = "create_game_object",
                    Alias = "landscape",
                    Name = string.IsNullOrWhiteSpace(request.Name) ? "Landscape" : request.Name.Trim(),
                    Properties = new Dictionary<string, JsonElement>(StringComparer.Ordinal)
                    {
                        ["position"] = JsonSerializer.SerializeToElement(new[]
                        {
                            request.PositionX, request.PositionY, request.PositionZ, 1.0f,
                        }),
                    },
                },
                new McpSceneOperation
                {
                    Kind = "add_component",
                    Alias = "landscapeComponent",
                    Target = "$landscape",
                    ComponentType = LandscapeComponentType,
                    Properties = properties,
                },
                new McpSceneOperation
                {
                    Kind = "select",
                    Target = "$landscape",
                },
                new McpSceneOperation
                {
                    Kind = "focus",
                    Target = "$landscape",
                },
            ];
        }
        else
        {
            operations =
            [
                new McpSceneOperation
                {
                    Kind = "update_component",
                    Target = request.TargetComponentId.Trim(),
                    Properties = properties,
                },
            ];
        }

        return true;
    }

    static bool TryParseShadowMode(string value, out float mode)
    {
        mode = value?.Trim().ToLowerInvariant() switch
        {
            "none" => 0.0f,
            "nearonly" or "near_only" or "near only" => 1.0f,
            "all" => 2.0f,
            _ => -1.0f,
        };
        return mode >= 0.0f;
    }

    static bool TryParseResidency(string value, out float mode)
    {
        mode = value?.Trim().ToLowerInvariant() switch
        {
            "persistent" => 0.0f,
            "grass" or "budgeted" or "budgetedgrass" or "budgeted_grass" => 1.0f,
            _ => -1.0f,
        };
        return mode >= 0.0f;
    }

    static bool TryParseSculptOperation(string value, out float operation)
    {
        operation = value?.Trim().ToLowerInvariant() switch
        {
            "raise" => 0.0f,
            "lower" => 1.0f,
            "smooth" => 2.0f,
            _ => -1.0f,
        };
        return operation >= 0.0f;
    }

    static bool IsFinitePositive(float value) => float.IsFinite(value) && value > 0.0f;
    static bool IsCoverage(float value) => float.IsFinite(value) && value is >= 0.0f and <= 1.0f;
    static bool AllFinite(params float[] values) => values.All(float.IsFinite);

    static bool Fail(string message, out string error)
    {
        error = message;
        return false;
    }
}
