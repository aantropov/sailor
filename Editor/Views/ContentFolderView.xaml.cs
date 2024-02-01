using System.Diagnostics;
using System.Runtime.InteropServices;
using Editor.Controls;
using Editor.Helpers;
using Editor.ViewModels;
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

            FolderTree.SelectedItemChanged += OnSelectTreeViewNode;
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
            var foldersModel = companyTreeViewBuilder.PopulateDirectory(service);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel);
            FolderTree.RootNodes = rootNodes;
        }
    }
}