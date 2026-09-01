using SailorEditor.Services;
using SailorEditor.Settings;
using SailorEngine;

namespace SailorEditor.Tests;

public sealed class GIProbesBakeBindingPolicyTests
{
    static readonly GIProbesCompositionIdentity TargetIdentity = new(10, 20, 30);

    [Fact]
    public void IncompatibleActiveStates_CompareTheFullCompositionIdentity()
    {
        GIProbesBindingCompositionState[] states =
        [
            new("Target", 1.0f, new(99, 99, 99)),
            new("Compatible", 1.0f, TargetIdentity),
            new("DifferentLayout", 1.0f, new(11, 20, 30)),
            new("DifferentRepresentation", 0.5f, new(10, 21, 30)),
            new("DifferentTransport", 2.0f, new(10, 20, 31)),
            new("Disabled", 0.0f, new(1, 2, 3))
        ];

        var incompatible = GIProbesBakeBindingPolicy.FindIncompatibleActiveStates(
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
        var incompatible = GIProbesBakeBindingPolicy.FindIncompatibleActiveStates(
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
            GIProbesBakeActivationState.Pending,
            GIProbesBakeBindingPolicy.AssessActivation(
                "Night", "{NIGHT}", true, baseline, pending).State);
        Assert.Equal(
            GIProbesBakeActivationState.Succeeded,
            GIProbesBakeBindingPolicy.AssessActivation(
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

        var assessment = GIProbesBakeBindingPolicy.AssessActivation(
            "Night",
            "{NIGHT}",
            true,
            baseline,
            rejected);

        Assert.Equal(GIProbesBakeActivationState.Rejected, assessment.State);
        Assert.Contains("preserved", assessment.Diagnostic, StringComparison.Ordinal);
    }

    [Fact]
    public void Activation_IsNotRequiredForRealtimeOrZeroWeightWorkflow()
    {
        var baseline = RuntimeState(compositions: 4, rejected: 2);

        var assessment = GIProbesBakeBindingPolicy.AssessActivation(
            "Night",
            "{NIGHT}",
            false,
            baseline,
            current: null);

        Assert.Equal(GIProbesBakeActivationState.NotRequired, assessment.State);
    }

    static GlobalIlluminationRuntimeState RuntimeState(
        ulong compositions,
        ulong rejected,
        IReadOnlyList<GlobalIlluminationProbeRuntimeState>? probes = null,
        string diagnostic = "") => new(
            MaxProbeStatesPerSnapshot: 4,
            Mode: GlobalIlluminationRuntimeMode.RealtimeAndBaked,
            Enabled: true,
            ProbeSource: GlobalIlluminationProbeSourceKind.BakedAssets,
            RuntimeSettings: new RuntimeGIProbesSettingsDescriptor(),
            RuntimeState: new RuntimeGIProbesRuntimeState(
                Lifecycle: RuntimeGIProbesLifecycleState.Disabled,
                Enabled: false,
                Paused: false,
                Throttled: false,
                PreviewEnabled: false,
                PreviewBudget: RuntimeGIProbesEditorBudget.Eco,
                SceneGeneration: 0,
                LightingGeneration: 0,
                PublishedRevision: 0,
                Capacity: 0,
                ActiveProbeCount: 0,
                ReadyProbeCount: 0,
                DirtyProbeCount: 0,
                QueuedProbeCount: 0,
                WorkerCount: 0,
                TracedRayCount: 0,
                PublishedBytes: 0,
                Coverage: 0,
                Refinement: 0,
                RaysPerSecond: 0,
                WorkerCpuMilliseconds: 0,
                LastPublicationMilliseconds: 0,
                Diagnostic: string.Empty),
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
