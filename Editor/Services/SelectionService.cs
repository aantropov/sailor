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
        long workspaceEpoch;
        CancellationTokenSource? pendingSelectionCancellation;
        int suppressRuntimeSelectionSync;
        int workspaceChangeInProgress;

        public event Action<InstanceId> OnSelectInstanceAction = delegate { };
        public event Action<ObservableObject> OnSelectAssetAction = delegate { };

        public SelectionService()
        {
            _selectionStore.Changed += snapshot =>
            {
                var selectedId = InstanceId.NullInstanceId;
                if (!string.IsNullOrWhiteSpace(snapshot.SelectedId))
                {
                    selectedId = new InstanceId(snapshot.SelectedId);
                }
                SelectedInstanceId = selectedId;
                SyncEditorSelectionToRuntime();
            };
        }

        public SelectionSnapshot Snapshot => _selectionStore.Current;
        public long WorkspaceEpoch => Interlocked.Read(ref workspaceEpoch);
        public bool IsWorkspaceResetInProgress => Volatile.Read(ref suppressRuntimeSelectionSync) != 0;
        public bool IsWorkspaceChangeInProgress => Volatile.Read(ref workspaceChangeInProgress) != 0;

        public void SelectInstance(InstanceId instanceId)
        {
            if (IsWorkspaceChangeInProgress)
                return;

            if (instanceId == null || instanceId.IsEmpty())
            {
                ClearSelection();
                return;
            }

            CancelPendingSelection();
            var worldService = MauiProgram.GetService<WorldService>();
            ObservableObject? next = null;
            if (worldService.TryGetGameObject(instanceId, out var gameObject))
            {
                next = gameObject;
                _selectionStore.Select(instanceId.Value, SelectionTargetKind.GameObject);
            }
            else if (worldService.TryGetComponent(instanceId, out var component))
            {
                next = component;
                _selectionStore.Select(instanceId.Value, SelectionTargetKind.Component);
            }
            else
            {
                return;
            }

            UpdateSelection(next, raiseInstanceAction: true, raiseAssetAction: false);
        }

        public async void SelectObject(ObservableObject obj, bool force = false)
        {
            if (IsWorkspaceChangeInProgress)
                return;

            if (obj == null)
            {
                ClearSelection();
                return;
            }

            if (!force && SelectedItems.Count == 1 && ReferenceEquals(SelectedItems[0], obj))
            {
                return;
            }

            var requestVersion = Interlocked.Increment(ref selectionRequestVersion);
            var requestEpoch = WorkspaceEpoch;
            var requestCancellation = new CancellationTokenSource();
            var previousCancellation = Interlocked.Exchange(ref pendingSelectionCancellation, requestCancellation);
            previousCancellation?.Cancel();
            _selectionStore.Select(TryGetSelectionId(obj), obj is Component ? SelectionTargetKind.Component : obj is GameObject ? SelectionTargetKind.GameObject : SelectionTargetKind.Asset);

            try
            {
                if (obj is AssetFile selectedAssetFile)
                {
                    await selectedAssetFile.PrepareInspectorResources().WaitAsync(requestCancellation.Token);
                    if (!IsCurrentRequest(requestVersion, requestEpoch, requestCancellation.Token))
                    {
                        return;
                    }
                }

                UpdateSelection(obj, raiseInstanceAction: obj is GameObject or Component, raiseAssetAction: true);

                if (obj is AssetFile assetFile)
                {
                    await assetFile.LoadDependentResources().WaitAsync(requestCancellation.Token);
                    if (IsCurrentRequest(requestVersion, requestEpoch, requestCancellation.Token) && ReferenceEquals(SelectedItem, obj))
                    {
                        UpdateSelection(obj, raiseInstanceAction: false, raiseAssetAction: true);
                        OnPropertyChanged(nameof(SelectedItem));
                    }
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

        public void ClearSelection()
        {
            CancelPendingSelection();
            ClearSelectionCore();
        }

        public void BeginWorkspaceChange()
        {
            if (Interlocked.Exchange(ref workspaceChangeInProgress, 1) != 0)
                return;

            Interlocked.Increment(ref workspaceEpoch);
            CancelPendingSelection();
        }

        public void ResetForWorkspaceChange()
        {
            BeginWorkspaceChange();
            Interlocked.Increment(ref suppressRuntimeSelectionSync);
            try
            {
                CancelPendingSelection();
                ClearSelectionCore();
            }
            finally
            {
                Interlocked.Decrement(ref suppressRuntimeSelectionSync);
            }
        }

        public void CompleteWorkspaceChange()
            => Interlocked.Exchange(ref workspaceChangeInProgress, 0);

        void ClearSelectionCore()
        {
            _selectionStore.Clear();
            SelectedItems.Clear();
            SelectedItem = null;
            SelectedInstanceId = InstanceId.NullInstanceId;
            OnSelectAssetAction?.Invoke(null);
        }

        void CancelPendingSelection()
        {
            Interlocked.Increment(ref selectionRequestVersion);
            Interlocked.Exchange(ref pendingSelectionCancellation, null)?.Cancel();
        }

        bool IsCurrentRequest(int requestVersion, long requestEpoch, CancellationToken cancellationToken) =>
            !cancellationToken.IsCancellationRequested
            && requestVersion == Volatile.Read(ref selectionRequestVersion)
            && requestEpoch == WorkspaceEpoch;

        void UpdateSelection(ObservableObject obj, bool raiseInstanceAction, bool raiseAssetAction)
        {
            SelectedItems.Clear();
            SelectedItems.Add(obj);
            SelectedItem = obj;

            if (raiseInstanceAction && SelectedInstanceId is not null && !SelectedInstanceId.IsEmpty())
                OnSelectInstanceAction?.Invoke(SelectedInstanceId);
            else if (SelectedInstanceId is null || SelectedInstanceId.IsEmpty())
                OnSelectInstanceAction?.Invoke(InstanceId.NullInstanceId);

            OnSelectAssetAction?.Invoke(raiseAssetAction ? obj : null);
        }

        void SyncEditorSelectionToRuntime()
        {
            if (Volatile.Read(ref suppressRuntimeSelectionSync) != 0)
                return;

            MauiProgram.GetService<EngineService>().UpdateEditorSelection([SelectedInstanceId]);
        }

        static string? TryGetSelectionId(ObservableObject obj) => obj switch
        {
            GameObject gameObject => gameObject.InstanceId?.Value,
            Component component => component.InstanceId?.Value,
            _ => null,
        };

        [ObservableProperty]
        private ObservableObject selectedItem;

        [ObservableProperty]
        private ObservableList<INotifyPropertyChanged> selectedItems = [];

        [ObservableProperty]
        private InstanceId selectedInstanceId = InstanceId.NullInstanceId;
    }
}
