using System.Diagnostics;
using System.Runtime.InteropServices;
using Editor.Helpers;
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
        }

        private void PopulateTreeView()
        {
            var xamlItemGroups = companyTreeViewBuilder.GroupData(service);
            var rootNodes = FolderTree.ProcessXamlItemGroups(xamlItemGroups);
            FolderTree.RootNodes = rootNodes;
        }
    }
}