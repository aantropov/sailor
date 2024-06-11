using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using SailorEditor.Services;

namespace SailorEditor.Views
{
    public partial class InspectorView : ContentView
    {
        InspectorTemplateSelector TemplateSelector { get; set; }

        public InspectorView()
        {
            InitializeComponent();

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectObjectAction += RefreshInspector;
            this.BindingContext = selectionViewModel;
        }

        private void RefreshInspector(object file)
        {
            // If we don't do that then TemplateSelector is not called
            var refresh = InspectedItem.ItemsSource;
            InspectedItem.ItemsSource = null;
            InspectedItem.ItemsSource = refresh;
        }
    }
}