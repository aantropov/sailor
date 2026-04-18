using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.ComponentModel;

namespace SailorEditor.Services
{
    public partial class SelectionService : ObservableObject
    {
        public event Action<InstanceId> OnSelectInstanceAction = delegate { };
        public event Action<ObservableObject> OnSelectAssetAction = delegate { };

        public void SelectInstance(InstanceId instanceId)
        {
            if (!instanceId.IsEmpty())
            {
                OnSelectInstanceAction?.Invoke(instanceId);
            }
        }

        public async void SelectObject(ObservableObject obj, bool force = false)
        {
            if (obj != null)
            {
                if (!force && SelectedItems.Count == 1 && ReferenceEquals(SelectedItems[0], obj))
                {
                    return;
                }

                if (obj is AssetFile assetFile)
                {
                    await assetFile.LoadDependentResources();
                }

                SelectedItems.Clear();
                SelectedItems.Add(obj);
                SelectedItem = obj;

                OnSelectAssetAction?.Invoke(obj);
            }
        }

        public void ClearSelection()
        {
            SelectedItems.Clear();
            SelectedItem = null;
            OnSelectAssetAction?.Invoke(null);
        }

        [ObservableProperty]
        private ObservableObject selectedItem;

        [ObservableProperty]
        private ObservableList<INotifyPropertyChanged> selectedItems = [];
    }
}
