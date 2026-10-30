using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;

namespace SailorEditor.Services;

public sealed partial class InspectorProjectionService : ObservableObject
{
    readonly SelectionService _selectionService;
    readonly WorldService _worldService;
    long _workspaceEpoch;
    long _resetGeneration;
    bool _refreshDeferred;
    AggregateInspectorModel? _observedAggregate;

    public long WorkspaceEpoch => Interlocked.Read(ref _workspaceEpoch);
    public long ResetGeneration => Interlocked.Read(ref _resetGeneration);
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
            if (args.PropertyName is nameof(SelectionService.InspectedItem) or nameof(SelectionService.InspectedItems))
            {
                Refresh();
            }
        };
        _worldService.OnUpdateWorldAction += _ => Refresh();

        Refresh();
    }

    public void Refresh()
    {
        using var perfScope = EditorPerf.Scope("InspectorProjectionService.Refresh");
        var suppressCommit = _selectionService.IsWorkspaceResetInProgress;
        if (!suppressCommit && SelectedItem is AggregateInspectorModel { HasInFlightInspectorCommit: true })
        {
            _refreshDeferred = true;
            return;
        }
        _refreshDeferred = false;
        if (suppressCommit)
        {
            Interlocked.Increment(ref _resetGeneration);
            IsWorkspaceResetInProgress = true;
        }

        try
        {
            var inspectedItems = ResolveInspectedItems();
            SelectedItem = inspectedItems.Count > 1
                ? new InspectorAggregateBuilder(
                    _worldService,
                    MauiProgram.GetService<AssetsService>(),
                    MauiProgram.GetService<EngineService>().EngineTypes,
                    MauiProgram.GetService<ITransactionScopeFactory>(),
                    MauiProgram.GetService<IActionContextProvider>()).Build(inspectedItems)
                : inspectedItems.FirstOrDefault();
            ObserveAggregate(SelectedItem as AggregateInspectorModel);
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
        Interlocked.Increment(ref _resetGeneration);
        IsWorkspaceResetInProgress = true;
        try
        {
            _refreshDeferred = false;
            ObserveAggregate(null);
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

    void ObserveAggregate(AggregateInspectorModel? aggregate)
    {
        if (_observedAggregate is not null)
        {
            _observedAggregate.CommitStateChanged -= OnAggregateCommitStateChanged;
        }
        _observedAggregate = aggregate;
        if (_observedAggregate is not null)
        {
            _observedAggregate.CommitStateChanged += OnAggregateCommitStateChanged;
        }
    }

    void OnAggregateCommitStateChanged()
    {
        if (_refreshDeferred && _observedAggregate is { HasInFlightInspectorCommit: false })
        {
            Refresh();
        }
    }

    IReadOnlyList<ObservableObject> ResolveInspectedItems()
    {
        var result = new List<ObservableObject>();
        foreach (var inspectedItem in _selectionService.InspectedItems)
        {
            var selectedInstanceId = inspectedItem switch
            {
                GameObject gameObject => gameObject.InstanceId,
                Component component => component.InstanceId,
                _ => null
            };
            if (selectedInstanceId is not null && !selectedInstanceId.IsEmpty())
            {
                if (_worldService.TryGetGameObject(selectedInstanceId, out var gameObject))
                {
                    result.Add(gameObject);
                }
                else if (_worldService.TryGetComponent(selectedInstanceId, out var component))
                {
                    result.Add(component);
                }
            }
            else
            {
                result.Add(inspectedItem);
            }
        }
        if (result.Count == 0 && _selectionService.InspectedItems.Count != 0)
        {
            _selectionService.InspectedItem = null;
        }
        return result;
    }

    GameObject? ResolveOwner(Component component) => _worldService.FindOwner(component);
}
