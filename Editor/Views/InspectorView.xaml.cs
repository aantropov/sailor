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
        readonly object inspectorCommitLock = new();
        bool isRefreshingInspector;
        bool inspectorRefreshRequested;
        Task<InspectorRefreshCommitOutcome>? inspectorCommitTask;
        long inspectorCommitResetGeneration;
        bool lifecycleSubscribed;

        public InspectorView()
        {
            InitializeComponent();

            inspectorProjection = MauiProgram.GetService<InspectorProjectionService>();
            inspectorPendingEditCoordinator = MauiProgram.GetService<InspectorPendingEditCoordinator>();
            this.BindingContext = inspectorProjection;
            QueueInspectorRefresh();

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
            QueueInspectorRefresh();
        }

        void UnsubscribeFromLifecycle()
        {
            if (!lifecycleSubscribed)
            {
                return;
            }

            inspectorPendingEditCoordinator.CommitPendingChangesRequested -= CommitPendingInspectorChanges;
            inspectorProjection.PropertyChanged -= OnProjectionChanged;
            lifecycleSubscribed = false;
            _ = CommitPendingInspectorChangesOnUnloadAsync();
        }

        async Task CommitPendingInspectorChangesOnUnloadAsync()
        {
            try
            {
                await CommitPendingInspectorChanges(
                    CancellationToken.None);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector pending edit commit failed while unloading: {ex}");
            }
        }

        void OnProjectionChanged(
            object sender,
            PropertyChangedEventArgs args)
        {
            if (args.PropertyName == nameof(InspectorProjectionService.SelectedItem) || args.PropertyName == nameof(InspectorProjectionService.Current))
            {
                QueueInspectorRefresh();
            }
        }

        void QueueInspectorRefresh()
        {
            inspectorRefreshRequested = true;
            if (isRefreshingInspector)
            {
                return;
            }

            isRefreshingInspector = true;
            _ = RefreshInspectorLoopSafelyAsync();
        }

        async Task RefreshInspectorLoopSafelyAsync()
        {
            try
            {
                while (inspectorRefreshRequested)
                {
                    inspectorRefreshRequested = false;
                    await RefreshInspectorAsync();
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector refresh failed: {ex}");
            }
            finally
            {
                isRefreshingInspector = false;
                if (inspectorRefreshRequested)
                {
                    QueueInspectorRefresh();
                }
            }
        }

        async Task RefreshInspectorAsync()
        {
            using var perfScope = EditorPerf.Scope("InspectorView.RefreshInspector");
            var resetGeneration = inspectorProjection.ResetGeneration;

            await InspectorRefreshCommitGate.TryApplyAsync(
                CommitPendingInspectorChangesForRefresh,
                ApplyInspectorRefresh,
                () => inspectorProjection.ResetGeneration !=
                    resetGeneration,
                CancellationToken.None);
        }

        void ApplyInspectorRefresh()
        {
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

        async Task<bool> CommitPendingInspectorChanges(
            CancellationToken cancellationToken)
        {
            if (inspectorProjection.IsWorkspaceResetInProgress)
            {
                return false;
            }

            var outcome = await CommitPendingInspectorChangesForRefresh(
                cancellationToken);
            return outcome.CanRefresh;
        }

        async Task<InspectorRefreshCommitOutcome>
            CommitPendingInspectorChangesForRefresh(
                CancellationToken cancellationToken)
        {
            var resetGeneration = inspectorProjection.ResetGeneration;
            if (inspectorProjection.IsWorkspaceResetInProgress)
            {
                return InspectorRefreshCommitOutcome.Ready;
            }

            if (InspectedItemHost.Content?.BindingContext is not
                    IInspectorEditable editable)
            {
                return InspectorRefreshCommitOutcome.Ready;
            }

            Task<InspectorRefreshCommitOutcome> commitTask;
            lock (inspectorCommitLock)
            {
                if (inspectorCommitTask is not null)
                {
                    if (inspectorCommitResetGeneration != resetGeneration)
                    {
                        inspectorCommitTask = null;
                        inspectorCommitResetGeneration = 0;
                        return InspectorRefreshCommitOutcome.Ready;
                    }

                    commitTask = inspectorCommitTask;
                }
                else
                {
                    if (!editable.HasPendingInspectorChanges)
                    {
                        return InspectorRefreshCommitOutcome.Ready;
                    }

                    commitTask = CommitInspectorEditableAsync(
                        editable,
                        cancellationToken);
                    inspectorCommitTask = commitTask;
                    inspectorCommitResetGeneration = resetGeneration;
                    _ = ClearInspectorCommitWhenCompleteAsync(
                        commitTask);
                }
            }

            try
            {
                var outcome =
                    await commitTask.WaitAsync(cancellationToken);
                return inspectorProjection.ResetGeneration == resetGeneration
                    ? outcome
                    : InspectorRefreshCommitOutcome.Ready;
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch when (
                inspectorProjection.ResetGeneration != resetGeneration)
            {
                return InspectorRefreshCommitOutcome.Ready;
            }
            finally
            {
                if (commitTask.IsCompleted)
                {
                    ClearInspectorCommit(commitTask);
                }
            }
        }

        static async Task<InspectorRefreshCommitOutcome>
            CommitInspectorEditableAsync(
            IInspectorEditable editable,
            CancellationToken cancellationToken)
        {
            var commitSucceeded = await editable.CommitInspectorChangesAsync(
                cancellationToken);
            return InspectorRefreshCommitOutcome.FromCompletedCommit(
                commitSucceeded,
                editable.HasPendingInspectorChanges);
        }

        async Task ClearInspectorCommitWhenCompleteAsync(
            Task<InspectorRefreshCommitOutcome> commitTask)
        {
            try
            {
                await commitTask;
            }
            catch
            {
                // The caller observes and reports the commit failure.
            }
            finally
            {
                ClearInspectorCommit(commitTask);
            }
        }

        void ClearInspectorCommit(
            Task<InspectorRefreshCommitOutcome> commitTask)
        {
            lock (inspectorCommitLock)
            {
                if (ReferenceEquals(
                        inspectorCommitTask,
                        commitTask))
                {
                    inspectorCommitTask = null;
                    inspectorCommitResetGeneration = 0;
                }
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
