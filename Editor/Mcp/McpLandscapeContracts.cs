#nullable enable

namespace SailorEditor.Mcp;

public sealed record McpLandscapeVegetationProfile
{
    public string ModelFileId { get; init; } = string.Empty;
    public string MaterialFileId { get; init; } = string.Empty;
    public int MeshIndex { get; init; } = -1;
    public uint InstancesPerChunk { get; init; } = 4;
    public float MinScale { get; init; } = 0.75f;
    public float MaxScale { get; init; } = 1.25f;
    public float GroundOffset { get; init; }
    public string Shadows { get; init; } = "NearOnly";
    public float ShadowDistance { get; init; } = 35.0f;
    public uint MinLod { get; init; }
    public uint MaxLod { get; init; } = 2;
    public float Lod1ScreenCoverage { get; init; } = 0.25f;
    public float Lod2ScreenCoverage { get; init; } = 0.05f;
    public float CullDistance { get; init; } = 120.0f;
    public float ColliderRadius { get; init; }
    public float ColliderHeight { get; init; } = 2.0f;
    public float ColliderOffsetY { get; init; } = 1.0f;
}

public sealed record McpLandscapeSculptStamp
{
    public float X { get; init; }
    public float Z { get; init; }
    public float Radius { get; init; } = 6.0f;
    public float Strength { get; init; } = 0.5f;
    public string Operation { get; init; } = "Raise";
}

public sealed record McpLandscapePaintStamp
{
    public float X { get; init; }
    public float Z { get; init; }
    public float Radius { get; init; } = 6.0f;
    public float Strength { get; init; } = 0.5f;
    public uint Layer { get; init; }
}

public sealed record McpLandscapeApplyRequest
{
    public bool Confirm { get; init; }
    public long? ExpectedWorkspaceEpoch { get; init; }
    public string? TargetComponentId { get; init; }
    public string Name { get; init; } = "Landscape";
    public float PositionX { get; init; }
    public float PositionY { get; init; }
    public float PositionZ { get; init; }
    public uint ChunksX { get; init; } = 4;
    public uint ChunksZ { get; init; } = 4;
    public float ChunkSize { get; init; } = 24.0f;
    public uint ChunkResolution { get; init; } = 24;
    public float HeightScale { get; init; } = 5.0f;
    public float NoiseScale { get; init; } = 0.035f;
    public uint Seed { get; init; } = 1337;
    public string MaterialFileId { get; init; } = string.Empty;
    public string[] LayerTextureFileIds { get; init; } = [];
    public string HeightmapTextureFileId { get; init; } = string.Empty;
    public string[] MaterialMaskFileIds { get; init; } = [];
    public float TextureTiling { get; init; } = 0.15f;
    public McpLandscapeVegetationProfile[] Vegetation { get; init; } = [];
    public McpLandscapeSculptStamp[] Sculpt { get; init; } = [];
    public McpLandscapePaintStamp[] Paint { get; init; } = [];
}
