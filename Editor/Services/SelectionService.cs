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

        public async void SelectObject(ObservableObject obj)
        {
            if (obj != null)
            {
                if (obj is AssetFile assetFile)
                {
                    await assetFile.LoadDependentResources();
                }

                SelectedItems.Clear();
                SelectedItems.Add(obj);

                OnSelectAssetAction?.Invoke(obj);
            }
        }

        [ObservableProperty]
        private ObservableList<INotifyPropertyChanged> selectedItems = [];
    }
}
