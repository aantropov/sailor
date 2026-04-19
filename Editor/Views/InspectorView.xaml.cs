using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using System.ComponentModel;

namespace SailorEditor.Views
{
    public partial class InspectorView : ContentView
    {
        readonly InspectorProjectionService inspectorProjection;

        public InspectorView()
        {
            InitializeComponent();

            inspectorProjection = MauiProgram.GetService<InspectorProjectionService>();
            inspectorProjection.PropertyChanged += OnProjectionChanged;
            this.BindingContext = inspectorProjection;
            RefreshInspector();
        }

        void OnProjectionChanged(object sender, PropertyChangedEventArgs args)
        {
            if (args.PropertyName == nameof(InspectorProjectionService.SelectedItem) || args.PropertyName == nameof(InspectorProjectionService.Current))
            {
                RefreshInspector();
            }
        }

        void RefreshInspector()
        {
            if (InspectedItemHost.Content?.BindingContext is IInspectorEditable editable && editable.HasPendingInspectorChanges)
            {
                editable.CommitInspectorChanges();
            }

            if (inspectorProjection.SelectedItem == null)
            {
                InspectedItemHost.Content = null;
                return;
            }

            var selector = (DataTemplateSelector)Resources["InspectorTemplateSelector"];
            var template = selector.SelectTemplate(inspectorProjection.SelectedItem, InspectedItemHost);
            var content = template.CreateContent();
            if (content is View view)
            {
                view.BindingContext = inspectorProjection.SelectedItem;
                InspectedItemHost.Content = view;
            }
        }
    }
}
