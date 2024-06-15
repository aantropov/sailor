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

                    if (assetFile is MaterialFile material)
                    {
                        var AssetService = MauiProgram.GetService<AssetsService>();

                        var preloadTasks = new List<Task>();
                        foreach (var tex in material.Samplers)
                        {
                            var task = Task.Run(async () =>
                            {
                                var file = AssetService.Files.Find((el) => el.FileId == tex.Value.Value);
                                if (file != null)
                                {
                                    await file.LoadDependentResources();
                                }
                            });

                            preloadTasks.Add(task);
                        }

                        await Task.WhenAll(preloadTasks);
                    }
                }

                SelectedItems.Clear();
                SelectedItems.Add(obj);

                OnSelectObjectAction?.Invoke(obj);
            }
        }

        [ObservableProperty]
        private ObservableList<INotifyPropertyChanged> selectedItems = new();
    }
}
