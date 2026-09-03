using CommunityToolkit.Mvvm.ComponentModel;
using SailorEngine;

namespace SailorEditor.ViewModels;

public enum GlobalIlluminationProbeMode
{
    Blend = 0,
    Additive
}

public enum GlobalIlluminationMode
{
    NoGI = 0,
    Runtime,
    Baked
}

public sealed class RuntimeGIProbesSettings
{
    public const uint CurrentVersion = 1;

    public uint Version { get; set; } = CurrentVersion;
    public bool IncludeSky { get; set; } = true;
    public bool IncludeEmissive { get; set; } = true;
    public bool IncludeDirectLighting { get; set; } = true;
    public uint BounceCount { get; set; } = 3;
    public float MinProbeSpacing { get; set; } = 1.0f;
    public float NormalBias { get; set; } = 0.05f;
    public float ViewBias { get; set; } = 0.05f;
    public float MaxRayDistance { get; set; } = 1000.0f;
}

public sealed partial class GlobalIlluminationProbeAssetReference : ObservableObject
{
    [ObservableProperty]
    FileId fileId = new();
}

public sealed partial class GlobalIlluminationProbeBinding : ObservableObject
{
    [ObservableProperty]
    GlobalIlluminationProbeAssetReference asset = new();

    [ObservableProperty]
    GlobalIlluminationProbeMode mode = GlobalIlluminationProbeMode.Blend;

    [ObservableProperty]
    float initialWeight;

    [ObservableProperty]
    bool preload;
}

public sealed class GISettings
{
    public GlobalIlluminationMode Mode { get; set; } =
        GlobalIlluminationMode.Baked;
    public RuntimeGIProbesSettings RuntimeProbes { get; set; } = new();
    public Dictionary<string, GlobalIlluminationProbeBinding> Probes { get; set; } = [];
}
