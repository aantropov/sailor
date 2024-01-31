using Editor.Models;
using Editor.Services;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Editor.Views
{
    public partial class InspectorView : ContentView
    {
        public InspectorView()
        {
            InitializeComponent();

            var selection = MauiProgram.GetService<SelectionService>();
            selection.OnSelectAssetAction += OnSelectAssetFile;
        }

        private void OnSelectAssetFile(AssetFile file)
        {
            SomeText.Text += "*";
        }
    }
}