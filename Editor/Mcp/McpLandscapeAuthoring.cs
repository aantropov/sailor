#nullable enable

using System.Text.Json;

namespace SailorEditor.Mcp;

internal static class McpLandscapeAuthoring
{
    const string LandscapeComponentType = "Sailor::LandscapeComponent";
    static readonly JsonSerializerOptions PropertyJson = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    public static bool TryBuildOperations(
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
        var profiles = new List<McpLandscapeVegetationProfile>(vegetation.Length);
        foreach (var profile in vegetation)
        {
            if (string.IsNullOrWhiteSpace(profile.ModelFileId))
            {
                return Fail("Every vegetation profile requires modelFileId; an empty materialFileId uses the GLB materials.", out error);
            }
            if (profile.InstancesPerChunk > 2048)
                return Fail("vegetation instancesPerChunk cannot exceed 2048.", out error);
            if (profile.MeshIndex is < -1 or > 65535)
                return Fail("vegetation meshIndex must be -1 (all meshes) or an index in 0..65535.", out error);
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
            if (!TryParseShadowMode(profile.ShadowMode, out var shadowMode))
                return Fail("vegetation shadowMode must be None, NearOnly, or All.", out error);
            if (shadowMode == "NearOnly" && !IsFinitePositive(profile.ShadowDistance))
                return Fail("NearOnly vegetation requires a positive shadowDistance.", out error);
            if (profile.MinLod > 15 || profile.MaxLod > 15 || profile.MaxLod < profile.MinLod)
                return Fail("vegetation LOD range must satisfy 0 <= minLod <= maxLod <= 15.", out error);
            if (profile.ScreenCoverageThresholds is null ||
                profile.ScreenCoverageThresholds.Any(value => !IsCoverage(value)) ||
                !profile.ScreenCoverageThresholds.SequenceEqual(profile.ScreenCoverageThresholds.OrderDescending()))
            {
                return Fail("vegetation screenCoverageThresholds must be in 0..1 and sorted in descending order.", out error);
            }
            if (!IsFinitePositive(profile.CullDistance))
                return Fail("vegetation cullDistance must be a finite positive number.", out error);
            if (!float.IsFinite(profile.ColliderRadius) || profile.ColliderRadius < 0.0f ||
                !float.IsFinite(profile.ColliderHeight) || profile.ColliderHeight < profile.ColliderRadius * 2.0f ||
                !float.IsFinite(profile.ColliderOffsetY))
            {
                return Fail("vegetation collider requires radius >= 0, height >= 2 * radius, and a finite Y offset.", out error);
            }
            profiles.Add(profile with { Residency = residencyMode, ShadowMode = shadowMode });
        }

        var sculptValues = new List<McpLandscapeSculptStamp>();
        foreach (var stamp in request.Sculpt ?? [])
        {
            if (!AllFinite(stamp.X, stamp.Z, stamp.Radius, stamp.Strength) ||
                stamp.Radius <= 0.0f || stamp.Strength < 0.0f)
            {
                return Fail("Every sculpt stamp requires finite coordinates, radius > 0 and strength >= 0.", out error);
            }
            if (!TryParseSculptOperation(stamp.Operation, out var operation))
                return Fail("sculpt operation must be Raise, Lower, or Flatten.", out error);
            sculptValues.Add(stamp with { Operation = operation });
        }

        var paintValues = new List<McpLandscapePaintStamp>();
        foreach (var stamp in request.Paint ?? [])
        {
            if (!AllFinite(stamp.X, stamp.Z, stamp.Radius, stamp.Strength) ||
                stamp.Radius <= 0.0f || stamp.Strength < 0.0f || stamp.Layer > 3)
            {
                return Fail("Every paint stamp requires finite coordinates, radius > 0, strength >= 0 and layer 0..3.", out error);
            }
            paintValues.Add(stamp);
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
            ["grassResidencyHysteresis"] = JsonSerializer.SerializeToElement(request.GrassResidencyHysteresis),
            ["vegetation"] = JsonSerializer.SerializeToElement(request.VegetationFileId ?? string.Empty),
            ["sculptStamps"] = JsonSerializer.SerializeToElement(sculptValues, PropertyJson),
            ["paintStamps"] = JsonSerializer.SerializeToElement(paintValues, PropertyJson),
            ["vegetationProfiles"] = JsonSerializer.SerializeToElement(profiles, PropertyJson),
            ["regenerate"] = JsonSerializer.SerializeToElement(false),
            ["flatten"] = JsonSerializer.SerializeToElement(false),
            ["saveVegetation"] = JsonSerializer.SerializeToElement(false),
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

    static bool TryParseShadowMode(string value, out string mode)
    {
        mode = value?.Trim().ToLowerInvariant() switch
        {
            "none" => "None",
            "nearonly" => "NearOnly",
            "all" => "All",
            _ => string.Empty,
        };
        return mode.Length != 0;
    }

    static bool TryParseResidency(string value, out string mode)
    {
        mode = value?.Trim().ToLowerInvariant() switch
        {
            "persistent" => "Persistent",
            "grass" => "Grass",
            _ => string.Empty,
        };
        return mode.Length != 0;
    }

    static bool TryParseSculptOperation(string value, out string operation)
    {
        operation = value?.Trim().ToLowerInvariant() switch
        {
            "raise" => "Raise",
            "lower" => "Lower",
            "flatten" => "Flatten",
            _ => string.Empty,
        };
        return operation.Length != 0;
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
