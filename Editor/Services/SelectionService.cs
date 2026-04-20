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
        int selectionRequestVersion = 0;

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

        public void SelectInstance(InstanceId instanceId)
        {
            if (instanceId == null || instanceId.IsEmpty())
            {
                ClearSelection();
                return;
            }

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
            if (obj == null)
            {
                ClearSelection();
                return;
            }

            if (!force && SelectedItems.Count == 1 && ReferenceEquals(SelectedItems[0], obj))
            {
                return;
            }

            var requestVersion = ++selectionRequestVersion;
            _selectionStore.Select(TryGetSelectionId(obj), obj is Component ? SelectionTargetKind.Component : obj is GameObject ? SelectionTargetKind.GameObject : SelectionTargetKind.Asset);

            if (obj is AssetFile selectedAssetFile)
            {
                await selectedAssetFile.PrepareInspectorResources();
                if (requestVersion != selectionRequestVersion)
                {
                    return;
                }
            }

            UpdateSelection(obj, raiseInstanceAction: obj is GameObject or Component, raiseAssetAction: true);

            if (obj is AssetFile assetFile)
            {
                await assetFile.LoadDependentResources();
                if (ReferenceEquals(SelectedItem, obj))
                {
                    UpdateSelection(obj, raiseInstanceAction: false, raiseAssetAction: true);
                    OnPropertyChanged(nameof(SelectedItem));
                }
            }
        }

        public void ClearSelection()
        {
            _selectionStore.Clear();
            SelectedItems.Clear();
            SelectedItem = null;
            SelectedInstanceId = InstanceId.NullInstanceId;
            OnSelectAssetAction?.Invoke(null);
        }

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
