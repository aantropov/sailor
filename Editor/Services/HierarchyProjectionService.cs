using CommunityToolkit.Mvvm.ComponentModel;
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

    public void Refresh()
    {
        var items = _worldService.Current.Prefabs
            .SelectMany(prefab => prefab.GameObjects.Select((gameObject, index) => new { prefab, gameObject, index }))
            .Select(x => new HierarchySourceItem(
                x.gameObject.InstanceId?.Value ?? string.Empty,
                x.gameObject.Name,
                x.gameObject.ParentIndex == uint.MaxValue ? null : x.prefab.GameObjects[(int)x.gameObject.ParentIndex].InstanceId?.Value))
            .Where(x => !string.IsNullOrWhiteSpace(x.Id));

        Roots = HierarchyProjectionBuilder.Build(items, _expandedIds, _selectionService.SelectedInstanceId?.Value);
    }
}
