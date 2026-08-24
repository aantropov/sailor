using SailorEngine;
using System.Numerics;

namespace SailorEditor.Services;

public enum ProbeVolumeBakeLifecycleState
{
    Idle = 0,
    Preparing,
    Baking,
    Saving,
    Succeeded,
    Failed,
    Cancelled
}

public sealed record ProbeVolumeBakeSettings(
    uint RaysPerProbe = 256,
    uint BounceCount = 3,
    uint RandomSeed = 0,
    uint MaxSubdivisionLevel = 3,
    float MinProbeSpacing = 1.0f,
    float NormalBias = 0.05f,
    float ViewBias = 0.05f,
    float MaxRayDistance = 1000.0f,
    bool IncludeSky = true,
    bool IncludeEmissive = true,
    bool IncludeDirectLighting = true)
{
    public const uint MaximumRaysPerProbe = 65536;
    public const uint MaximumBounceCount = 64;
    public const uint MaximumSubdivisionLevel = 16;
}

public sealed record ProbeVolumeBakeRequest(
    FileId WorldAsset,
    string OutputVirtualPath,
    string StateName,
    ProbeVolumeBakeSettings Settings,
    FileId? LayoutSource = null,
    bool AutoBounds = true,
    Vector3 VolumeMin = default,
    Vector3 VolumeMax = default,
    Vector3 FallbackEnvironment = default,
    bool Overwrite = false,
    uint ThreadCount = 1)
{
    public const uint MaximumThreadCount = 64;
}

public sealed record ProbeVolumeBakeStatus(
    ProbeVolumeBakeLifecycleState State,
    float Progress,
    uint CompletedProbes,
    uint TotalProbes,
    uint BrickCount,
    uint ProbeCount,
    float ElapsedSeconds,
    ulong LayoutHash,
    ulong TransportHash,
    ulong LightingHash,
    string Stage,
    string OutputVirtualPath,
    string Diagnostic)
{
    public bool IsRunning => State is
        ProbeVolumeBakeLifecycleState.Preparing or
        ProbeVolumeBakeLifecycleState.Baking or
        ProbeVolumeBakeLifecycleState.Saving;
}

public enum GlobalIlluminationCompositionMode
{
    Blend = 0,
    Additive
}

public enum GlobalIlluminationRuntimeMode
{
    Realtime = 0,
    RealtimeAndBaked,
    BakedOnly
}

public sealed record GlobalIlluminationBindingDescriptor(
    string Name,
    FileId Asset,
    GlobalIlluminationCompositionMode Mode,
    float InitialWeight,
    bool Preload);

public enum GlobalIlluminationResidency
{
    Unloaded = 0,
    Loading,
    Resident,
    Failed
}

public sealed record GlobalIlluminationProbeRuntimeState(
    string Name,
    FileId Asset,
    GlobalIlluminationCompositionMode Mode,
    float Weight,
    GlobalIlluminationResidency Residency,
    ulong AssetRevision,
    string Diagnostic);

public sealed record GlobalIlluminationRuntimeState(
    uint MaxProbeStatesPerSnapshot,
    GlobalIlluminationRuntimeMode Mode,
    bool Enabled,
    IReadOnlyList<GlobalIlluminationProbeRuntimeState> Probes,
    string Diagnostic,
    ulong CompositionCount,
    ulong RejectedCompositionCount);
