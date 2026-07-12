#nullable enable

namespace SailorEngine;

public sealed class EngineTypeMetadataContract
{
    public uint? MetadataVersion { get; set; }
    public string? ModuleName { get; set; }
    public long? TimeStamp { get; set; }
    public List<EngineTypeMetadataType> EngineTypes { get; set; } = [];
    public List<EngineTypeMetadataDefaults> Cdos { get; set; } = [];
    public List<Dictionary<string, List<string>>> Enums { get; set; } = [];
    public List<EngineTypeMetadataAssetType> AssetTypes { get; set; } = [];
}

public sealed class EngineTypeMetadataType
{
    public string Typename { get; set; } = string.Empty;
    public string Base { get; set; } = string.Empty;
    public Dictionary<string, string> Properties { get; set; } = [];
}

public sealed class EngineTypeMetadataDefaults
{
    public string Typename { get; set; } = string.Empty;
    public Dictionary<string, object> DefaultValues { get; set; } = [];
}

public sealed class EngineTypeMetadataAssetType
{
    public string Typename { get; set; } = string.Empty;
    public List<string> Extensions { get; set; } = [];
    public object? Properties { get; set; }
}

public sealed class EngineTypeMetadataAssetProperty
{
    public string Name { get; set; } = string.Empty;
    public string Type { get; set; } = string.Empty;
}
