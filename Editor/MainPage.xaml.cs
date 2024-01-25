using Editor.Helpers;
using Editor.Services;
using System.Diagnostics;

namespace Editor
{
    public partial class MainPage : ContentPage
    {
        AssetsService service;
        FolderTreeViewBuilder companyTreeViewBuilder;
        public MainPage(AssetsService service, FolderTreeViewBuilder companyTreeViewBuilder)
        {
            InitializeComponent();

            this.service = service;
            this.companyTreeViewBuilder = companyTreeViewBuilder;
        }
    }
}