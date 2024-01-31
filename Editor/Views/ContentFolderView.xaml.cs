using System.Diagnostics;
using System.Runtime.InteropServices;
using Editor.Helpers;
using Editor.Models;
using Editor.Services;

namespace Editor.Views
{
    public partial class ContentFolderView : ContentView
    {
        AssetsService service;
        FolderTreeViewBuilder companyTreeViewBuilder;
        public ContentFolderView()
        {
            InitializeComponent();

            this.service = MauiProgram.GetService<AssetsService>();
            this.companyTreeViewBuilder = MauiProgram.GetService<FolderTreeViewBuilder>();

            PopulateTreeView();

            FolderTree.SelectedItemChanged += OnSelectAssetFile;
        }

        public static void OnSelectAssetFile(object sender, EventArgs e)
        {
            var assetFile = sender as AssetFile;
            if (assetFile != null)
            { 
                MauiProgram.GetService<SelectionService>().OnAssetSelect(assetFile);
            }
        }

        private void PopulateTreeView()
        {
            var foldersModel = companyTreeViewBuilder.PopulateDirectory(service);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel);
            FolderTree.RootNodes = rootNodes;
        }
    }
}