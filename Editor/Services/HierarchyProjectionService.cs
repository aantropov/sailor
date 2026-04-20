using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;
using SailorEngine;

namespace SailorEditor.Services;

public sealed partial class HierarchyProjectionService : ObservableObject
{
    readonly WorldService _worldService;
    readonly SelectionService _selectionService;
    readonly HashSet<string> _expandedIds = [];

    [ObservableProperty]
    IReadOnlyList<HierarchyProjectionNode> roots = Array.Empty<HierarchyProjectionNode>();

    [ObservableProperty]
    IReadOnlyList<HierarchyListRow> visibleRows = Array.Empty<HierarchyListRow>();

    public HierarchyProjectionService(WorldService worldService, SelectionService selectionService)
    {
        _worldService = worldService;
        _selectionService = selectionService;

        _worldService.OnUpdateWorldAction += _ => Refresh();
        _selectionService.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(SelectionService.SelectedInstanceId))
                Refresh();
        };
    }

    public void SetExpanded(InstanceId instanceId, bool isExpanded)
    {
        if (instanceId is null || instanceId.IsEmpty())
            return;

        if (isExpanded)
            _expandedIds.Add(instanceId.Value);
        else
            _expandedIds.Remove(instanceId.Value);

        Refresh();
    }

    public void EnsureVisible(InstanceId instanceId)
    {
        if (instanceId is null || instanceId.IsEmpty())
            return;

        var parentById = _worldService.Current.Prefabs
            .SelectMany(prefab => prefab.GameObjects.Select(gameObject => new { gameObject, parentId = ResolveParentId(prefab, gameObject) }))
            .Where(x => x.gameObject.InstanceId is not null && !x.gameObject.InstanceId.IsEmpty())
            .ToDictionary(x => x.gameObject.InstanceId!.Value, x => x.parentId, StringComparer.Ordinal);

        var currentId = instanceId.Value;
        while (parentById.TryGetValue(currentId, out var parentId) && !string.IsNullOrWhiteSpace(parentId))
        {
            _expandedIds.Add(parentId);
            currentId = parentId;
        }

        Refresh();
    }

    public void Refresh()
    {
        using var perfScope = EditorPerf.Scope("HierarchyProjectionService.Refresh");

        var items = _worldService.Current.Prefabs
            .SelectMany(prefab => prefab.GameObjects.Select((gameObject, index) => new { prefab, gameObject, index }))
            .Select(x => new HierarchySourceItem(
                x.gameObject.InstanceId?.Value ?? string.Empty,
                x.gameObject.Name,
                ResolveParentId(x.prefab, x.gameObject)))
            .Where(x => !string.IsNullOrWhiteSpace(x.Id));

        Roots = HierarchyProjectionBuilder.Build(items, _expandedIds, _selectionService.SelectedInstanceId?.Value);
        VisibleRows = HierarchyProjectionBuilder.Flatten(Roots);
    }

    static string? ResolveParentId(Prefab prefab, GameObject gameObject)
    {
        if (gameObject.ParentIndex == uint.MaxValue || gameObject.ParentIndex >= prefab.GameObjects.Count)
            return null;

        var parent = prefab.GameObjects[(int)gameObject.ParentIndex];
        return parent.InstanceId is not null && !parent.InstanceId.IsEmpty() ? parent.InstanceId.Value : null;
    }
}
