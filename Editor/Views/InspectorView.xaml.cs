using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using SailorEditor.Services;
using System.ComponentModel;

namespace SailorEditor.Views
{
    public partial class InspectorView : ContentView
    {
        readonly SelectionService selectionViewModel;

        public InspectorView()
        {
            InitializeComponent();

            selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.PropertyChanged += OnSelectionChanged;
            this.BindingContext = selectionViewModel;
            RefreshInspector();
        }

        void OnSelectionChanged(object sender, PropertyChangedEventArgs args)
        {
            if (args.PropertyName == nameof(SelectionService.SelectedItem))
            {
                RefreshInspector();
            }
        }

        void RefreshInspector()
        {
            if (selectionViewModel.SelectedItem == null)
            {
                InspectedItemHost.Content = null;
                return;
            }

            var selector = (DataTemplateSelector)Resources["InspectorTemplateSelector"];
            var template = selector.SelectTemplate(selectionViewModel.SelectedItem, InspectedItemHost);
            var content = template.CreateContent();
            if (content is View view)
            {
                view.BindingContext = selectionViewModel.SelectedItem;
                InspectedItemHost.Content = view;
            }
        }
    }
}
