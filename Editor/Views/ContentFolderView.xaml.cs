using System.Diagnostics;
using System.Runtime.InteropServices;
using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using SailorEditor.Services;

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
            selectionViewModel.OnSelectAssetAction += OnSelectAssetFile;
        }

        private void OnSelectAssetFile(AssetFile file)
        {
            var assetFile = FolderTree.SelectedItem.BindingContext as TreeViewItem<AssetFile>;

            if (assetFile != null)
            {
                if (assetFile.Model.UID != file.UID)
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

        public static void OnSelectTreeViewNode(object sender, EventArgs args)
        {
            var selectionChanged = args as TreeView.OnSelectItemEventArgs;
            var assetFile = selectionChanged.Model as TreeViewItem<AssetFile>;
            if (assetFile != null)
            {
                MauiProgram.GetService<SelectionService>().OnSelectAsset(assetFile.Model);
            }
        }
        private void PopulateTreeView()
        {
            var foldersModel = FolderTreeViewBuilder.PopulateDirectory(service);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel);
            FolderTree.RootNodes = rootNodes;
        }
    }
}