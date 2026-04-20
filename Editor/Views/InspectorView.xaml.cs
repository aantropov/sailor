using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor.Utility;
using SailorEngine;
using Component = SailorEditor.ViewModels.Component;
using PropertyChangedEventArgs = System.ComponentModel.PropertyChangedEventArgs;

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
            using var perfScope = EditorPerf.Scope("InspectorView.RefreshInspector");

            if (InspectedItemHost.Content?.BindingContext is IInspectorEditable editable && editable.HasPendingInspectorChanges)
            {
                editable.CommitInspectorChanges();
            }

            if (inspectorProjection.SelectedItem == null)
            {
                InspectedItemHost.Content = null;
                return;
            }

            if (InspectedItemHost.Content is View existingView && AreEquivalentSelection(existingView.BindingContext, inspectorProjection.SelectedItem))
            {
                existingView.BindingContext = inspectorProjection.SelectedItem;
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

        static bool AreEquivalentSelection(object? current, object? next)
        {
            if (current is AssetFile || next is AssetFile)
            {
                return false;
            }

            if (ReferenceEquals(current, next))
            {
                return true;
            }

            if (current is GameObject or Component || next is GameObject or Component)
            {
                return false;
            }

            return TryGetInstanceId(current) is { } currentId &&
                   TryGetInstanceId(next) is { } nextId &&
                   current.GetType() == next.GetType() &&
                   currentId == nextId;
        }

        static InstanceId? TryGetInstanceId(object? item) => item switch
        {
            GameObject gameObject => gameObject.InstanceId,
            Component component => component.InstanceId,
            _ => null,
        };
    }
}
