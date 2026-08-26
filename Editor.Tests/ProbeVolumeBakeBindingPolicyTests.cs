using SailorEditor.Services;
using SailorEngine;

namespace SailorEditor.Tests;

public sealed class ProbeVolumeBakeBindingPolicyTests
{
    static readonly ProbeVolumeCompositionIdentity TargetIdentity = new(10, 20, 30);

    [Fact]
    public void IncompatibleActiveStates_CompareTheFullCompositionIdentity()
    {
        ProbeVolumeBindingCompositionState[] states =
        [
            new("Target", 1.0f, new(99, 99, 99)),
            new("Compatible", 1.0f, TargetIdentity),
            new("DifferentLayout", 1.0f, new(11, 20, 30)),
            new("DifferentRepresentation", 0.5f, new(10, 21, 30)),
            new("DifferentTransport", 2.0f, new(10, 20, 31)),
            new("Disabled", 0.0f, new(1, 2, 3))
        ];

        var incompatible = ProbeVolumeBakeBindingPolicy.FindIncompatibleActiveStates(
            "Target",
            1.0f,
            TargetIdentity,
            states);

        Assert.Equal(
            ["DifferentLayout", "DifferentRepresentation", "DifferentTransport"],
            incompatible);
    }

    [Fact]
    public void ZeroWeightTarget_DoesNotDeactivateExistingLighting()
    {
        var incompatible = ProbeVolumeBakeBindingPolicy.FindIncompatibleActiveStates(
            "Target",
            0.0f,
            TargetIdentity,
            [new("Existing", 1.0f, new(1, 2, 3))]);

        Assert.Empty(incompatible);
    }

    [Fact]
    public void Activation_SucceedsOnlyAfterFreshResidentComposition()
    {
        var baseline = RuntimeState(compositions: 4, rejected: 2);
        var pending = RuntimeState(
            compositions: 4,
            rejected: 2,
            probes: [Probe("Night", "{NIGHT}", GlobalIlluminationResidency.Resident)]);
        var active = RuntimeState(
            compositions: 5,
            rejected: 2,
            probes: [Probe("Night", "{NIGHT}", GlobalIlluminationResidency.Resident)]);

        Assert.Equal(
            ProbeVolumeBakeActivationState.Pending,
            ProbeVolumeBakeBindingPolicy.AssessActivation(
                "Night", "{NIGHT}", true, baseline, pending).State);
        Assert.Equal(
            ProbeVolumeBakeActivationState.Succeeded,
            ProbeVolumeBakeBindingPolicy.AssessActivation(
                "Night", "{NIGHT}", true, baseline, active).State);
    }

    [Fact]
    public void Activation_ReportsPreservedSnapshotAsRejection()
    {
        var baseline = RuntimeState(compositions: 4, rejected: 2);
        var rejected = RuntimeState(
            compositions: 4,
            rejected: 3,
            diagnostic: "preserved the last complete snapshot: probe layout hashes differ");

        var assessment = ProbeVolumeBakeBindingPolicy.AssessActivation(
            "Night",
            "{NIGHT}",
            true,
            baseline,
            rejected);

        Assert.Equal(ProbeVolumeBakeActivationState.Rejected, assessment.State);
        Assert.Contains("preserved", assessment.Diagnostic, StringComparison.Ordinal);
    }

    [Fact]
    public void Activation_IsNotRequiredForRealtimeOrZeroWeightWorkflow()
    {
        var baseline = RuntimeState(compositions: 4, rejected: 2);

        var assessment = ProbeVolumeBakeBindingPolicy.AssessActivation(
            "Night",
            "{NIGHT}",
            false,
            baseline,
            current: null);

        Assert.Equal(ProbeVolumeBakeActivationState.NotRequired, assessment.State);
    }

    static GlobalIlluminationRuntimeState RuntimeState(
        ulong compositions,
        ulong rejected,
        IReadOnlyList<GlobalIlluminationProbeRuntimeState>? probes = null,
        string diagnostic = "") => new(
            MaxProbeStatesPerSnapshot: 4,
            Mode: GlobalIlluminationRuntimeMode.RealtimeAndBaked,
            Enabled: true,
            Probes: probes ?? [],
            Diagnostic: diagnostic,
            CompositionCount: compositions,
            RejectedCompositionCount: rejected);

    static GlobalIlluminationProbeRuntimeState Probe(
        string name,
        string asset,
        GlobalIlluminationResidency residency) => new(
            name,
            new FileId(asset),
            GlobalIlluminationCompositionMode.Blend,
            1.0f,
            residency,
            AssetRevision: 1,
            Diagnostic: string.Empty);
}
