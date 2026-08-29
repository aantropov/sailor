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
    Realtime = 0,
    RealtimeAndBaked,
    BakedOnly
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
        GlobalIlluminationMode.RealtimeAndBaked;
    public Dictionary<string, GlobalIlluminationProbeBinding> Probes { get; set; } = [];
}
