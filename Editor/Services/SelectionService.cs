using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System.ComponentModel;

namespace SailorEditor.Services
{
    public partial class SelectionService : ObservableObject
    {
        public event Action<ObservableObject> OnSelectObjectAction = delegate { };

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

                OnSelectObjectAction?.Invoke(obj);
            }
        }

        [ObservableProperty]
        private ObservableList<INotifyPropertyChanged> selectedItems = [];
    }
}
