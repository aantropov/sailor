using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.UI.Xaml.Controls.Primitives;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.IO;
using static System.Net.Mime.MediaTypeNames;

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

            if (assetFile is MaterialFile material)
            {
                var AssetService = MauiProgram.GetService<AssetsService>();

                var preloadTasks = new List<Task>();
                foreach (var tex in material.Samplers)
                {
                    var task = Task.Run(async () =>
                     {
                         var file = AssetService.Files.Find((el) => el.UID == tex.Value);
                         if (file != null)
                         {
                             await file.PreloadResources(false);
                         }
                     });

                    preloadTasks.Add(task);
                }

                await Task.WhenAll(preloadTasks);
            }

            SelectedItems.Clear();
            SelectedItems.Add(assetFile);

            OnSelectAssetAction?.Invoke(assetFile);
        }
    }
}
