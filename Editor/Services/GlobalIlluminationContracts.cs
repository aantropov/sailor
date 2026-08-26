using SailorEngine;
using System.Globalization;
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
    uint MaxSubdivisionLevel = 16,
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

public static class GlobalIlluminationBindingInputPolicy
{
    public static bool TryParseInitialWeight(
        string text,
        out float initialWeight)
    {
        return float.TryParse(
                text,
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out initialWeight) &&
            float.IsFinite(initialWeight) &&
            initialWeight >= 0.0f;
    }
}

public readonly record struct ProbeVolumeCompositionIdentity(
    ulong LayoutHash,
    ulong RepresentationHash,
    ulong TransportHash)
{
    public bool IsValid =>
        LayoutHash != 0 &&
        RepresentationHash != 0 &&
        TransportHash != 0;
}

public sealed record ProbeVolumeBindingCompositionState(
    string Name,
    float Weight,
    ProbeVolumeCompositionIdentity Identity);

public enum ProbeVolumeBakeActivationState
{
    NotRequired = 0,
    Pending,
    Succeeded,
    Rejected
}

public sealed record ProbeVolumeBakeActivationAssessment(
    ProbeVolumeBakeActivationState State,
    string Diagnostic);

public static class ProbeVolumeBakeBindingPolicy
{
    public static IReadOnlyList<string> FindIncompatibleActiveStates(
        string targetName,
        float targetWeight,
        ProbeVolumeCompositionIdentity targetIdentity,
        IReadOnlyCollection<ProbeVolumeBindingCompositionState> states)
    {
        ArgumentNullException.ThrowIfNull(states);
        if (string.IsNullOrWhiteSpace(targetName))
            throw new ArgumentException("The baked probe state name is empty.", nameof(targetName));
        if (!float.IsFinite(targetWeight) || targetWeight < 0.0f)
            throw new ArgumentOutOfRangeException(nameof(targetWeight));
        if (!targetIdentity.IsValid)
            throw new ArgumentException("The baked probe state has an invalid composition identity.", nameof(targetIdentity));
        if (targetWeight == 0.0f)
            return [];

        var incompatible = new HashSet<string>(StringComparer.Ordinal);
        foreach (var state in states)
        {
            if (string.IsNullOrWhiteSpace(state.Name))
                throw new ArgumentException("A bound probe state name is empty.", nameof(states));
            if (!float.IsFinite(state.Weight) || state.Weight < 0.0f)
                throw new ArgumentException($"Probe state '{state.Name}' has an invalid weight.", nameof(states));
            if (state.Weight == 0.0f ||
                string.Equals(state.Name, targetName, StringComparison.Ordinal))
            {
                continue;
            }
            if (!state.Identity.IsValid)
                throw new ArgumentException($"Probe state '{state.Name}' has an invalid composition identity.", nameof(states));
            if (state.Identity != targetIdentity)
                incompatible.Add(state.Name);
        }

        return incompatible.Order(StringComparer.Ordinal).ToArray();
    }

    public static ProbeVolumeBakeActivationAssessment AssessActivation(
        string targetName,
        string targetAssetFileId,
        bool activationRequired,
        GlobalIlluminationRuntimeState baseline,
        GlobalIlluminationRuntimeState? current)
    {
        ArgumentNullException.ThrowIfNull(baseline);
        if (!activationRequired)
        {
            return new ProbeVolumeBakeActivationAssessment(
                ProbeVolumeBakeActivationState.NotRequired,
                "Runtime activation is not required for the current GI mode or zero binding weight.");
        }
        if (string.IsNullOrWhiteSpace(targetName) ||
            string.IsNullOrWhiteSpace(targetAssetFileId))
        {
            throw new ArgumentException("The baked probe state identity is incomplete.");
        }
        if (current is null)
        {
            return new ProbeVolumeBakeActivationAssessment(
                ProbeVolumeBakeActivationState.Pending,
                "Global Illumination runtime state is unavailable.");
        }
        if (current.RejectedCompositionCount > baseline.RejectedCompositionCount)
        {
            return new ProbeVolumeBakeActivationAssessment(
                ProbeVolumeBakeActivationState.Rejected,
                string.IsNullOrWhiteSpace(current.Diagnostic)
                    ? "Global Illumination rejected the baked probe composition."
                    : current.Diagnostic);
        }

        var target = current.Probes.FirstOrDefault(probe =>
            string.Equals(probe.Name, targetName, StringComparison.Ordinal));
        if (target?.Residency == GlobalIlluminationResidency.Failed)
        {
            return new ProbeVolumeBakeActivationAssessment(
                ProbeVolumeBakeActivationState.Rejected,
                string.IsNullOrWhiteSpace(target.Diagnostic)
                    ? $"Probe state '{targetName}' failed to become resident."
                    : target.Diagnostic);
        }
        if (current.CompositionCount <= baseline.CompositionCount ||
            current.Mode == GlobalIlluminationRuntimeMode.Realtime ||
            !current.Enabled ||
            target is null ||
            !string.Equals(
                target.Asset?.Value,
                targetAssetFileId,
                StringComparison.Ordinal) ||
            target.Weight <= 0.0f ||
            target.Residency != GlobalIlluminationResidency.Resident)
        {
            return new ProbeVolumeBakeActivationAssessment(
                ProbeVolumeBakeActivationState.Pending,
                current.Diagnostic ?? string.Empty);
        }

        return new ProbeVolumeBakeActivationAssessment(
            ProbeVolumeBakeActivationState.Succeeded,
            current.Diagnostic ?? string.Empty);
    }
}

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
