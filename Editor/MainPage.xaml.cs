using SailorEditor.Helpers;
using SailorEditor.Services;
using System.Diagnostics;

namespace SailorEditor
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