using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor.Utility;
using SailorEditor.Workflow;
using SailorEngine;
using Component = SailorEditor.ViewModels.Component;
using PropertyChangedEventArgs = System.ComponentModel.PropertyChangedEventArgs;

namespace SailorEditor.Views
{
    public partial class InspectorView : ContentView
    {
        readonly InspectorProjectionService inspectorProjection;
        readonly InspectorPendingEditCoordinator inspectorPendingEditCoordinator;
        bool isRefreshingInspector;
        bool isCommittingInspectorChanges;
        bool lifecycleSubscribed;

        public InspectorView()
        {
            InitializeComponent();

            inspectorProjection = MauiProgram.GetService<InspectorProjectionService>();
            inspectorPendingEditCoordinator = MauiProgram.GetService<InspectorPendingEditCoordinator>();
            this.BindingContext = inspectorProjection;
            RefreshInspector();

            Loaded += (_, _) => SubscribeToLifecycle();
            Unloaded += (_, _) => UnsubscribeFromLifecycle();
        }

        void SubscribeToLifecycle()
        {
            if (lifecycleSubscribed)
            {
                return;
            }

            inspectorPendingEditCoordinator.CommitPendingChangesRequested += CommitPendingInspectorChanges;
            inspectorProjection.PropertyChanged += OnProjectionChanged;
            lifecycleSubscribed = true;
            RefreshInspector();
        }

        void UnsubscribeFromLifecycle()
        {
            if (!lifecycleSubscribed)
            {
                return;
            }

            try
            {
                CommitPendingInspectorChanges();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector pending edit commit failed while unloading: {ex}");
            }

            inspectorPendingEditCoordinator.CommitPendingChangesRequested -= CommitPendingInspectorChanges;
            inspectorProjection.PropertyChanged -= OnProjectionChanged;
            lifecycleSubscribed = false;
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

            if (isRefreshingInspector)
            {
                return;
            }

            isRefreshingInspector = true;

            try
            {
                CommitPendingInspectorChanges();

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
                    view.IsEnabled = inspectorProjection.SelectedItem is not AssetFile { IsReadOnly: true };
                    InspectedItemHost.Content = view;
                }
            }
            finally
            {
                isRefreshingInspector = false;
            }
        }

        bool CommitPendingInspectorChanges()
        {
            if (inspectorProjection.IsWorkspaceResetInProgress)
            {
                return false;
            }

            if (isCommittingInspectorChanges ||
                InspectedItemHost.Content?.BindingContext is not IInspectorEditable editable ||
                !editable.HasPendingInspectorChanges)
            {
                return true;
            }

            isCommittingInspectorChanges = true;
            try
            {
                editable.CommitInspectorChanges();
            }
            finally
            {
                isCommittingInspectorChanges = false;
            }

            return !editable.HasPendingInspectorChanges;
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

            return InspectorSelectionIdentity.AreEquivalent(
                current?.GetType(),
                TryGetInstanceId(current)?.Value,
                next?.GetType(),
                TryGetInstanceId(next)?.Value);
        }

        static InstanceId? TryGetInstanceId(object? item) => item switch
        {
            GameObject gameObject => gameObject.InstanceId,
            Component component => component.InstanceId,
            _ => null,
        };
    }
}
