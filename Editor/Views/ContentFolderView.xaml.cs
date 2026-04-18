using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using SailorEditor.Services;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;

namespace SailorEditor.Views
{
    public partial class ContentFolderView : ContentView
    {
        AssetsService service;
        public ContentFolderView()
        {
            InitializeComponent();

            this.service = MauiProgram.GetService<AssetsService>();

            PopulateTreeView();

            FolderTree.SelectedItemChanged += OnSelectTreeViewNode;
            FolderTree.DropRequested += OnContentDropRequested;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectAssetAction += SelectAssetFile;
        }

        private void SelectAssetFile(ObservableObject obj)
        {
            if (obj is AssetFile file)
            {
                if (FolderTree.SelectedItem != null)
                {
                    var assetFile = FolderTree.SelectedItem.BindingContext as TreeViewItem<AssetFile>;

                    if (assetFile.Model.FileId == file.FileId)
                    {
                        return;
                    }
                }

                foreach (var el in FolderTree.RootNodes)
                {
                    var res = el.FindFileRecursive(file);

                    if (res != null)
                    {
                        res.Select();
                        FolderTree.SelectedItem = res;
                        break;
                    }
                }
            }
        }

        public static void OnSelectTreeViewNode(object sender, EventArgs args)
        {
            var selectionChanged = args as TreeView.OnSelectItemEventArgs;
            if (selectionChanged.Model is TreeViewItem<AssetFile> assetFile)
            {
                MauiProgram.GetService<SelectionService>().SelectObject(assetFile.Model);
            }
        }

        private async void OnContentDropRequested(object sender, TreeViewDropRequest request)
        {
            if (request.Source is not GameObject gameObject)
            {
                return;
            }

            switch (request.Target)
            {
                case null:
                    service.CreatePrefabAsset(null, gameObject);
                    PopulateTreeView();
                    request.Handled = true;
                    break;

                case AssetFolder folder:
                    service.CreatePrefabAsset(folder, gameObject);
                    PopulateTreeView();
                    request.Handled = true;
                    break;

                case PrefabFile prefab:
                    request.Handled = true;
                    var overwrite = await Application.Current.MainPage.DisplayAlert(
                        "Overwrite prefab",
                        $"Overwrite {prefab.DisplayName} with {gameObject.DisplayName}?",
                        "Overwrite",
                        "Cancel");

                    if (overwrite)
                    {
                        service.CreatePrefabAsset(null, gameObject, overwrite: true, existingPrefab: prefab);
                        PopulateTreeView();
                    }
                    break;
            }
        }

        private void PopulateTreeView()
        {
            var foldersModel = FolderTreeViewBuilder.PopulateDirectory(service);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel, new TreeViewPopulateArgs());
            FolderTree.RootNodes = rootNodes;
        }
    }
}
