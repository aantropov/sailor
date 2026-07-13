using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;

namespace SailorEditor.Services;

public sealed partial class InspectorProjectionService : ObservableObject
{
    readonly SelectionService _selectionService;
    readonly WorldService _worldService;
    long _workspaceEpoch;

    public long WorkspaceEpoch => Interlocked.Read(ref _workspaceEpoch);
    public bool IsWorkspaceResetInProgress { get; private set; }

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

        Refresh();
    }

    public void Refresh()
    {
        using var perfScope = EditorPerf.Scope("InspectorProjectionService.Refresh");
        var suppressCommit = _selectionService.IsWorkspaceResetInProgress;
        if (suppressCommit)
            IsWorkspaceResetInProgress = true;

        try
        {
            SelectedItem = ResolveSelectedItem();
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

            OnPropertyChanged(nameof(SelectedItem));
            OnPropertyChanged(nameof(Current));
        }
        finally
        {
            if (suppressCommit)
                IsWorkspaceResetInProgress = false;
        }
    }

    public void ResetForWorkspaceChange()
    {
        Interlocked.Increment(ref _workspaceEpoch);
        IsWorkspaceResetInProgress = true;
        try
        {
            SelectedItem = null;
            Current = InspectorProjection.Empty;
            OnPropertyChanged(nameof(SelectedItem));
            OnPropertyChanged(nameof(Current));
        }
        finally
        {
            IsWorkspaceResetInProgress = false;
        }
    }

    object? ResolveSelectedItem()
    {
        var selectedInstanceId = _selectionService.SelectedInstanceId;
        if (selectedInstanceId is not null && !selectedInstanceId.IsEmpty())
        {
            if (_worldService.TryGetGameObject(selectedInstanceId, out var gameObject))
                return gameObject;

            if (_worldService.TryGetComponent(selectedInstanceId, out var component))
                return component;
        }

        return _selectionService.SelectedItem;
    }

    GameObject? ResolveOwner(Component component) => _worldService.FindOwner(component);
}
