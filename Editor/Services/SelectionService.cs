using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;
using SailorEngine;
using Component = SailorEditor.ViewModels.Component;
using INotifyPropertyChanged = System.ComponentModel.INotifyPropertyChanged;

namespace SailorEditor.Services
{
    public partial class SelectionService : ObservableObject
    {
        readonly SelectionStore _selectionStore = new();
        int selectionRequestVersion;
        int inspectionRebindVersion;
        long workspaceEpoch;
        CancellationTokenSource? pendingSelectionCancellation;
        int suppressRuntimeSelectionSync;
        int workspaceResetInProgress;
        int workspaceChangeInProgress;
        IReadOnlyList<ObservableObject> inspectedItems = Array.Empty<ObservableObject>();
        ObservableObject? inspectedItem;

        public event Action<InstanceId> OnSelectInstanceAction = delegate { };
        public event Action<ObservableObject> OnSelectAssetAction = delegate { };

        public SelectionService()
        {
            _selectionStore.Changed += snapshot =>
            {
                SelectedInstanceId = snapshot.Kind is SelectionTargetKind.GameObject or SelectionTargetKind.Component &&
                    !string.IsNullOrWhiteSpace(snapshot.SelectedId)
                        ? new InstanceId(snapshot.SelectedId)
                        : InstanceId.NullInstanceId;
                OnPropertyChanged(nameof(Snapshot));
                OnPropertyChanged(nameof(SelectedInstanceIds));
                SyncEditorSelectionToRuntime();
            };
        }

        public SelectionSnapshot Snapshot => _selectionStore.Current;
        public IReadOnlyList<InstanceId> SelectedInstanceIds =>
            Snapshot.Kind is SelectionTargetKind.GameObject or SelectionTargetKind.Component
                ? Snapshot.SelectedIds.Select(id => new InstanceId(id)).ToArray()
                : Array.Empty<InstanceId>();
        public IReadOnlyList<ObservableObject> InspectedItems => inspectedItems;
        public ObservableObject? InspectedItem
        {
            get => inspectedItem;
            set => SetInspectedSelection(value is null ? [] : [value]);
        }
        public long WorkspaceEpoch => Interlocked.Read(ref workspaceEpoch);
        public bool IsWorkspaceResetInProgress => Volatile.Read(ref workspaceResetInProgress) != 0;
        public bool IsWorkspaceChangeInProgress => Volatile.Read(ref workspaceChangeInProgress) != 0;

        public void SelectInstance(InstanceId instanceId)
        {
            if (IsWorkspaceChangeInProgress)
            {
                return;
            }
            if (instanceId is null || instanceId.IsEmpty())
            {
                ClearSelection();
                return;
            }
            var target = ResolveSceneTarget(instanceId);
            if (target is not null)
            {
                SelectInstances([instanceId]);
            }
        }

        public void SelectInstances(IEnumerable<InstanceId> instanceIds)
        {
            if (IsWorkspaceChangeInProgress)
            {
                return;
            }
            var targets = instanceIds.Where(id => id is not null && !id.IsEmpty())
                .Reverse().DistinctBy(id => id.Value, StringComparer.Ordinal).Reverse()
                .Select(ResolveSceneTarget).OfType<ObservableObject>().ToArray();
            CancelPendingSelection();
            if (targets.Length == 0)
            {
                ClearSelectionCore();
                return;
            }
            var changed = SetSelectionSnapshot(targets);
            UpdateSelection(targets, changed, raiseAssetAction: false);
        }

        public async void SelectObject(ObservableObject obj, bool force = false)
            => await SelectObjectAsync(obj, force);

        public Task SelectObjectAsync(ObservableObject obj, bool force = false) =>
            SelectObjectsAsync(obj is null ? [] : [obj], force);

        public async Task SelectObjectsAsync(IEnumerable<ObservableObject> objects, bool force = false)
        {
            if (IsWorkspaceChangeInProgress)
            {
                return;
            }
            var targets = objects.Where(item => item is not null).Reverse()
                .DistinctBy(item => (item.GetType(), TryGetSelectionId(item) ??
                    System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(item).ToString()))
                .Reverse().ToArray();
            if (targets.Length == 0)
            {
                ClearSelection();
                return;
            }
            if (!force && pendingSelectionCancellation is null && SelectedItems.Count == targets.Length &&
                Snapshot.SelectedIds.SequenceEqual(targets.Select(TryGetSelectionId), StringComparer.Ordinal) &&
                SelectedItems.Zip(targets).All(pair => ReferenceEquals(pair.First, pair.Second)))
            {
                return;
            }

            var requestVersion = Interlocked.Increment(ref selectionRequestVersion);
            var requestEpoch = WorkspaceEpoch;
            var requestCancellation = new CancellationTokenSource();
            var previousCancellation = Interlocked.Exchange(ref pendingSelectionCancellation, requestCancellation);
            previousCancellation?.Cancel();
            var selectionChanged = SetSelectionSnapshot(targets);
            try
            {
                foreach (var asset in targets.OfType<AssetFile>())
                {
                    await asset.EnsureMetadataLoadedAsync(requestCancellation.Token);
                    if (!IsCurrentRequest(requestVersion, requestEpoch, requestCancellation.Token))
                    {
                        return;
                    }
                    if (targets.Length == 1)
                    {
                        await asset.PrepareInspectorResources().WaitAsync(requestCancellation.Token);
                        if (!IsCurrentRequest(requestVersion, requestEpoch, requestCancellation.Token))
                        {
                            return;
                        }
                    }
                }
                UpdateSelection(targets, selectionChanged, raiseAssetAction: true);
                foreach (var asset in targets.OfType<AssetFile>())
                {
                    if (asset is ModelFile model)
                    {
                        await model.LoadDependentResources(requestCancellation.Token);
                    }
                    else
                    {
                        await asset.LoadDependentResources().WaitAsync(requestCancellation.Token);
                    }
                    if (!IsCurrentRequest(requestVersion, requestEpoch, requestCancellation.Token))
                    {
                        return;
                    }
                }
                if (targets.Any(item => item is AssetFile) &&
                    IsCurrentRequest(requestVersion, requestEpoch, requestCancellation.Token))
                {
                    OnPropertyChanged(nameof(InspectedItems));
                    OnPropertyChanged(nameof(InspectedItem));
                    OnPropertyChanged(nameof(SelectedItem));
                    OnSelectAssetAction?.Invoke(targets[^1]);
                }
            }
            catch (OperationCanceledException) when (requestCancellation.IsCancellationRequested)
            {
                // A newer selection or workspace activation owns the projection now.
            }
            finally
            {
                Interlocked.CompareExchange(ref pendingSelectionCancellation, null, requestCancellation);
                requestCancellation.Dispose();
            }
        }

        public async Task<bool> NavigateToReferenceAsync(object? reference, CancellationToken cancellationToken = default)
        {
            if (IsWorkspaceChangeInProgress || reference is null)
            {
                return false;
            }
            reference = reference switch
            {
                Observable<FileId> observableFileId => observableFileId.Value,
                Observable<InstanceId> observableInstanceId => observableInstanceId.Value,
                ObjectPtr objectPtr when !objectPtr.FileId.IsEmpty() => objectPtr.FileId,
                ObjectPtr objectPtr => objectPtr.InstanceId,
                _ => reference,
            };
            var worldService = MauiProgram.GetService<WorldService>();
            ObservableObject? target = reference switch
            {
                FileId fileId when !fileId.IsEmpty() =>
                    await MauiProgram.GetService<AssetsService>().ResolveAssetAsync(fileId, cancellationToken),
                InstanceId instanceId when !instanceId.IsEmpty() => ResolveSceneReferenceTarget(worldService, instanceId),
                Component component => worldService.FindOwner(component),
                GameObject gameObject => gameObject,
                AssetFile assetFile => assetFile,
                _ => null,
            };
            if (target is null)
            {
                return false;
            }
            await SelectObjectAsync(target);
            return true;
        }

        public void RebindSceneSelection()
        {
            if (IsWorkspaceChangeInProgress)
            {
                return;
            }
            if (Snapshot.Kind is SelectionTargetKind.GameObject or SelectionTargetKind.Component)
            {
                var targets = SelectedInstanceIds.Select(ResolveSceneTarget).OfType<ObservableObject>().ToArray();
                CancelPendingSelection();
                SetSelectionSnapshot(targets);
                UpdateSelection(targets, raiseInstanceAction: true, raiseAssetAction: false);
            }
            else if (InspectedItems.Any(item => item is GameObject or Component))
            {
                var rebound = InspectedItems.Select(item => item switch
                {
                    GameObject gameObject => ResolveSceneTarget(gameObject.InstanceId),
                    Component component => ResolveSceneTarget(component.InstanceId),
                    _ => item,
                }).OfType<ObservableObject>().ToArray();
                SetInspectedSelection(rebound);
            }
        }

        public void ClearSelection()
        {
            CancelPendingSelection();
            ClearSelectionCore();
        }

        internal async Task RebindInspectedAssetAsync(AssetFile previous, AssetFile? replacement)
        {
            var epoch = WorkspaceEpoch;
            if (!InspectedItems.Contains(previous) || IsWorkspaceChangeInProgress)
            {
                return;
            }
            var requestVersion = Interlocked.Increment(ref inspectionRebindVersion);
            if (replacement is not null)
            {
                await replacement.EnsureMetadataLoadedAsync();
                if (InspectedItems.Count == 1)
                {
                    await replacement.PrepareInspectorResources();
                }
                await replacement.LoadDependentResources();
            }
            if (requestVersion == Volatile.Read(ref inspectionRebindVersion) && epoch == WorkspaceEpoch &&
                !IsWorkspaceChangeInProgress && InspectedItems.Contains(previous))
            {
                SetInspectedSelection(InspectedItems.Select(item => ReferenceEquals(item, previous) ? replacement : item)
                    .OfType<ObservableObject>().ToArray());
            }
        }

        public void ApplyRuntimeSelection(InstanceId? instanceId)
        {
            ApplyRuntimeSelectionSnapshot(instanceId is null || instanceId.IsEmpty() ? [] : [instanceId]);
        }

        public void ApplyRuntimeSelectionSnapshot(IEnumerable<InstanceId> instanceIds, bool preserveInspectedItem = true)
        {
            if (IsWorkspaceChangeInProgress)
            {
                return;
            }
            var ids = instanceIds.ToArray();
            if (ids.Length != 0)
            {
                var worldService = MauiProgram.GetService<WorldService>();
                if (ids.Any(id => id is null || id.IsEmpty() || !worldService.TryGetGameObject(id, out _)))
                {
                    return;
                }
            }

            Interlocked.Increment(ref suppressRuntimeSelectionSync);
            try
            {
                if (ids.Length == 0)
                {
                    CancelPendingSelection();
                    ClearSelectionCore(preserveInspectedItem);
                }
                else
                {
                    SelectInstances(ids);
                }
            }
            finally
            {
                Interlocked.Decrement(ref suppressRuntimeSelectionSync);
            }
        }

        public void ResetForDocumentChange()
        {
            Interlocked.Increment(ref workspaceResetInProgress);
            Interlocked.Increment(ref suppressRuntimeSelectionSync);
            try
            {
                CancelPendingSelection();
                ClearSelectionCore();
            }
            finally
            {
                Interlocked.Decrement(ref suppressRuntimeSelectionSync);
                Interlocked.Decrement(ref workspaceResetInProgress);
            }
        }

        public void BeginWorkspaceChange()
        {
            if (Interlocked.Exchange(ref workspaceChangeInProgress, 1) != 0)
            {
                return;
            }
            Interlocked.Increment(ref workspaceEpoch);
            SailorEditor.Scene.SceneViewportAssetDropPayload.ResetSelections();
            CancelPendingSelection();
        }

        public void ResetForWorkspaceChange()
        {
            BeginWorkspaceChange();
            Interlocked.Increment(ref workspaceResetInProgress);
            Interlocked.Increment(ref suppressRuntimeSelectionSync);
            try
            {
                CancelPendingSelection();
                ClearSelectionCore();
            }
            finally
            {
                Interlocked.Decrement(ref suppressRuntimeSelectionSync);
                Interlocked.Decrement(ref workspaceResetInProgress);
            }
        }

        public void CompleteWorkspaceChange() => Interlocked.Exchange(ref workspaceChangeInProgress, 0);

        void ClearSelectionCore(bool preserveInspectedItem = false)
        {
            _selectionStore.Clear();
            SelectedItems.Clear();
            SelectedItem = null;
            SelectedInstanceId = InstanceId.NullInstanceId;
            OnPropertyChanged(nameof(SelectedItems));
            if (!preserveInspectedItem)
            {
                SetInspectedSelection([]);
            }
            OnSelectAssetAction?.Invoke(null);
        }

        void CancelPendingSelection()
        {
            Interlocked.Increment(ref selectionRequestVersion);
            Interlocked.Exchange(ref pendingSelectionCancellation, null)?.Cancel();
        }

        bool IsCurrentRequest(int requestVersion, long requestEpoch, CancellationToken cancellationToken) =>
            !cancellationToken.IsCancellationRequested &&
            requestVersion == Volatile.Read(ref selectionRequestVersion) && requestEpoch == WorkspaceEpoch;

        bool SetSelectionSnapshot(IReadOnlyList<ObservableObject> targets) =>
            _selectionStore.SelectMany(targets.Select(TryGetSelectionId).OfType<string>(), targets.LastOrDefault() switch
            {
                GameObject => SelectionTargetKind.GameObject,
                Component => SelectionTargetKind.Component,
                AssetFile => SelectionTargetKind.Asset,
                _ => SelectionTargetKind.None,
            });

        void UpdateSelection(IReadOnlyList<ObservableObject> targets, bool raiseInstanceAction, bool raiseAssetAction)
        {
            SelectedItems.Clear();
            foreach (var target in targets)
            {
                SelectedItems.Add(target);
            }
            SelectedItem = targets.LastOrDefault();
            SetInspectedSelection(targets);
            OnPropertyChanged(nameof(SelectedItems));
            if (raiseInstanceAction || SelectedInstanceId.IsEmpty())
            {
                OnSelectInstanceAction?.Invoke(SelectedInstanceId);
            }
            OnSelectAssetAction?.Invoke(raiseAssetAction ? SelectedItem : null);
        }

        void SetInspectedSelection(IReadOnlyList<ObservableObject> targets)
        {
            if (inspectedItems.SequenceEqual(targets, ReferenceEqualityComparer.Instance))
            {
                return;
            }
            inspectedItems = Array.AsReadOnly(targets.ToArray());
            inspectedItem = targets.LastOrDefault();
            OnPropertyChanged(nameof(InspectedItems));
            OnPropertyChanged(nameof(InspectedItem));
        }

        void SyncEditorSelectionToRuntime()
        {
            if (Volatile.Read(ref suppressRuntimeSelectionSync) == 0)
            {
                _ = SyncEditorSelectionToRuntimeAsync();
            }
        }

        async Task SyncEditorSelectionToRuntimeAsync()
        {
            try
            {
                await MauiProgram.GetService<EngineService>().UpdateEditorSelectionAsync(SelectedInstanceIds);
            }
            catch (Exception exception)
            {
                Console.WriteLine($"[SelectionService] Failed to synchronize runtime selection: {exception.Message}");
            }
        }

        static ObservableObject? ResolveSceneTarget(InstanceId instanceId)
        {
            var worldService = MauiProgram.GetService<WorldService>();
            if (worldService.TryGetGameObject(instanceId, out var gameObject))
            {
                return gameObject;
            }
            return worldService.TryGetComponent(instanceId, out var component) ? component : null;
        }

        static GameObject? ResolveSceneReferenceTarget(WorldService worldService, InstanceId instanceId)
        {
            if (worldService.TryGetGameObject(instanceId, out var gameObject))
            {
                return gameObject;
            }
            return worldService.TryGetComponent(instanceId, out var component) ? worldService.FindOwner(component) : null;
        }

        static string? TryGetSelectionId(ObservableObject obj) => obj switch
        {
            GameObject gameObject => gameObject.InstanceId?.Value,
            Component component => component.InstanceId?.Value,
            AssetFile assetFile => assetFile.FileId?.Value,
            _ => null,
        };

        [ObservableProperty]
        private ObservableObject? selectedItem;

        [ObservableProperty]
        private ObservableList<INotifyPropertyChanged> selectedItems = [];

        [ObservableProperty]
        private InstanceId selectedInstanceId = InstanceId.NullInstanceId;
    }
}
