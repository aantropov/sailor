using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.UI.Xaml.Controls.Primitives;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.IO;

namespace SailorEditor.Services
{
    public partial class SelectionService : ObservableObject
    {
        public event Action<AssetFile> OnSelectAssetAction = delegate { };

        [ObservableProperty]
        private ObservableList<INotifyPropertyChanged> selectedItems = new();

        public async void OnSelectAsset(AssetFile assetFile)
        {
            await assetFile.PreloadResources(false);

            SelectedItems.Clear();
            SelectedItems.Add(assetFile);

            OnSelectAssetAction?.Invoke(assetFile);
        }
    }
}
