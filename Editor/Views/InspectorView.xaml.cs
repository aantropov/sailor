using Editor.Helpers;
using Editor.ViewModels;
using Editor.Services;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Editor.Views
{
    public partial class InspectorView : ContentView
    {
        InspectorTemplateSelector TemplateSelector { get; set; }

        public InspectorView()
        {
            InitializeComponent();

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectAssetAction += OnSelectAssetFile;

            this.BindingContext = selectionViewModel;
        }

        private void OnSelectAssetFile(AssetFile file)
        {
            OnPropertyChanged();
        }
    }
}