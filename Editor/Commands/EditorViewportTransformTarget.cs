using SailorEditor.Services;
using SailorEngine;

namespace SailorEditor.Commands;

internal sealed class EditorViewportTransformTarget(
    EngineService engineService,
    WorldService worldService) : IAlreadyAppliedTransformTarget
{
    public Task<bool> ApplyLocalAsync(
        string instanceId,
        string yaml,
        CancellationToken cancellationToken = default)
    {
        var id = new InstanceId(instanceId);
        engineService.InvalidateQueuedWorldSnapshots();
        return EditorViewportTransformApplication.ApplyAlreadyCommittedAsync(
            () => worldService.ApplyGameObjectYamlLocal(id, yaml),
            engineService.RefreshCurrentWorldAsync,
            cancellationToken);
    }

    public Task<bool> CommitAndApplyLocalAsync(
        string instanceId,
        string yaml,
        CancellationToken cancellationToken = default)
    {
        var id = new InstanceId(instanceId);
        return EditorViewportTransformApplication.CommitAndApplyAsync(
            token => engineService.CommitChangesAsync(id, yaml, token),
            () => worldService.ApplyGameObjectYamlLocal(id, yaml),
            engineService.RefreshCurrentWorldAsync,
            cancellationToken);
    }
}
