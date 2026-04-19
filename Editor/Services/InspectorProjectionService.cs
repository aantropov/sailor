using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;

namespace SailorEditor.Services;

public sealed partial class InspectorProjectionService : ObservableObject
{
    readonly SelectionService _selectionService;
    readonly WorldService _worldService;

    [ObservableProperty]
    InspectorProjection current = InspectorProjection.Empty;

    [ObservableProperty]
    object? selectedItem;

    public InspectorProjectionService(SelectionService selectionService, WorldService worldService)
    {
        _selectionService = selectionService;
        _worldService = worldService;

        _selectionService.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(SelectionService.SelectedItem) || args.PropertyName == nameof(SelectionService.SelectedInstanceId))
                Refresh();
        };
        _worldService.OnUpdateWorldAction += _ => Refresh();
    }

    public void Refresh()
    {
        SelectedItem = _selectionService.SelectedItem;
        Current = SelectedItem switch
        {
            GameObject gameObject => InspectorProjectionBuilder.Build(
                gameObject.InstanceId?.Value,
                SelectionTargetKind.GameObject,
                gameObject.Name,
                gameObject.InstanceId?.Value,
                _worldService.GetComponents(gameObject).Select(component => new InspectorComponentProjection(component.InstanceId?.Value ?? string.Empty, component.DisplayName ?? component.Typename?.Name ?? string.Empty, component.Typename?.Name ?? string.Empty))),
            Component component => InspectorProjectionBuilder.Build(
                component.InstanceId?.Value,
                SelectionTargetKind.Component,
                component.DisplayName ?? component.Typename?.Name,
                ResolveOwner(component)?.InstanceId?.Value,
                Array.Empty<InspectorComponentProjection>()),
            _ => InspectorProjection.Empty
        };
    }

    GameObject? ResolveOwner(Component component)
    {
        foreach (var prefab in _worldService.Current.Prefabs)
        {
            foreach (var gameObject in prefab.GameObjects)
            {
                if (_worldService.GetComponents(gameObject).Any(x => x.InstanceId == component.InstanceId))
                    return gameObject;
            }
        }

        return null;
    }
}
