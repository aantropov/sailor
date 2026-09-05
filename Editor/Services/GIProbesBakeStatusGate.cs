namespace SailorEditor.Services;

public sealed class GIProbesBakeStatusGate
{
    public bool IsPreservingTerminalStatus { get; private set; }

    public void BeginLaunch()
    {
        IsPreservingTerminalStatus = false;
    }

    public void PreserveTerminalStatus()
    {
        IsPreservingTerminalStatus = true;
    }

    public bool ShouldApplyPolledStatus(bool isRunning)
    {
        if (isRunning)
        {
            IsPreservingTerminalStatus = false;
            return true;
        }

        return !IsPreservingTerminalStatus;
    }
}
