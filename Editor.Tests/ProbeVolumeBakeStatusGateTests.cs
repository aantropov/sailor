using SailorEditor.Services;

namespace SailorEditor.Tests;

public sealed class ProbeVolumeBakeStatusGateTests
{
    [Fact]
    public void LaunchFailure_PreservesDiagnosticInsteadOfApplyingIdlePoll()
    {
        var gate = new ProbeVolumeBakeStatusGate();

        gate.BeginLaunch();
        gate.PreserveTerminalStatus();

        Assert.False(gate.ShouldApplyPolledStatus(isRunning: false));
        Assert.True(gate.IsPreservingTerminalStatus);
    }

    [Fact]
    public void RunningBake_ReleasesPreservedDiagnostic()
    {
        var gate = new ProbeVolumeBakeStatusGate();
        gate.PreserveTerminalStatus();

        Assert.True(gate.ShouldApplyPolledStatus(isRunning: true));
        Assert.False(gate.IsPreservingTerminalStatus);
    }

    [Fact]
    public void NewLaunch_ReleasesPreviousTerminalDiagnostic()
    {
        var gate = new ProbeVolumeBakeStatusGate();
        gate.PreserveTerminalStatus();

        gate.BeginLaunch();

        Assert.True(gate.ShouldApplyPolledStatus(isRunning: false));
    }
}
