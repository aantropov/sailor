using SailorEditor.Services;
using SailorEngine;

namespace SailorEditor.Commands;

internal sealed class EditorViewportTransformTarget(
    EngineService engineService,
    WorldService worldService) : IAlreadyAppliedTransformTarget
{
    public bool ApplyLocal(string instanceId, string yaml)
    {
        var id = new InstanceId(instanceId);
        engineService.InvalidateQueuedWorldSnapshots();
        return EditorViewportTransformApplication.ApplyAlreadyCommitted(
            () => worldService.ApplyGameObjectYamlLocal(id, yaml),
            engineService.RefreshCurrentWorld);
    }

    public bool CommitAndApplyLocal(string instanceId, string yaml)
    {
        var id = new InstanceId(instanceId);
        return EditorViewportTransformApplication.CommitAndApply(
            () => engineService.CommitChanges(id, yaml),
            () => worldService.ApplyGameObjectYamlLocal(id, yaml),
            engineService.RefreshCurrentWorld);
    }
}
