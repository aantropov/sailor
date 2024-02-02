using Editor.ViewModels;
using System.Collections.ObjectModel;
using System.IO;

namespace Editor.Services
{
    public class SelectionService
    {
        public event Action<AssetFile> OnSelectAssetAction = delegate { };

        public ObservableCollection<object> SelectedItems
        {
            get => selectedItems;
            set
            {
                if (selectedItems == value)
                    return;

                selectedItems = value;
            }
        }

        ObservableCollection<object> selectedItems = new();

        public void OnSelectAsset(AssetFile assetFile)
        {
            assetFile.PreloadResources();

            SelectedItems.Clear();
            SelectedItems.Add(assetFile);

            OnSelectAssetAction?.Invoke(assetFile);
        }
    }
}
