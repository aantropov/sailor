using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using SailorEditor.Services;
using CommunityToolkit.Mvvm.ComponentModel;

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

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectObjectAction += SelectAssetFile;
        }

        private void SelectAssetFile(ObservableObject obj)
        {
            if (obj is AssetFile file)
            {
                if (FolderTree.SelectedItem.BindingContext is TreeViewItem<AssetFile> assetFile)
                {
                    if (assetFile.Model.FileId != file.FileId)
                    {
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

        private void PopulateTreeView()
        {
            var foldersModel = FolderTreeViewBuilder.PopulateDirectory(service);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel, new TreeViewPopulateArgs());
            FolderTree.RootNodes = rootNodes;
        }
    }
}