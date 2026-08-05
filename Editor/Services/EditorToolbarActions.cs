using SailorEditor.Commands;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.ViewModels;
using SailorEngine;
using ViewModelComponent = SailorEditor.ViewModels.Component;
using ViewModelGameObject = SailorEditor.ViewModels.GameObject;

namespace SailorEditor.Services
{
    internal sealed class EditorToolbarActions
    {
        readonly EngineService engineService;
        readonly SemaphoreSlim simulationGate = new(1, 1);
        int isSimulating;

        public EditorToolbarActions(EngineService engineService)
        {
            this.engineService = engineService ??
                throw new ArgumentNullException(nameof(engineService));
            engineService.OnEditorSimulationStateChanged +=
                SetSimulationState;
            engineService.OnLifecycleStateChanged += state =>
            {
                if (state != EngineLifecycleState.Running)
                    SetSimulationState(false);
            };
        }

        public bool IsSimulating => Volatile.Read(ref isSimulating) != 0;
        public event Action<bool> SimulationStateChanged = delegate { };

        public async Task SaveAsync()
        {
            if (await engineService.GetEditorSimulationStateAsync())
            {
                await DisplayStatus(
                    "Save Scene",
                    "Stop simulation before saving the scene.");
                return;
            }

            var selectionService = MauiProgram.GetService<SelectionService>();
            var selected = selectionService.SelectedItem;

            if (selected is IInspectorEditable editable && editable.HasPendingInspectorChanges)
            {
                if (!await editable.CommitInspectorChangesAsync())
                {
                    await DisplayStatus("Save Scene", "Inspector changes could not be committed.");
                    return;
                }
            }

            var result = await MauiProgram.GetService<WorldService>().SaveCurrentWorldAsync();
            switch (result.Outcome)
            {
                case SceneSaveOutcome.Saved:
                    await DisplayStatus("Save Scene", $"Saved scene: {result.Path}");
                    break;
                case SceneSaveOutcome.Cancelled:
                    await DisplayStatus("Save Scene", "Scene save cancelled.");
                    break;
                default:
                    await DisplayStatus("Save Scene", result.Error ?? "Scene save failed.");
                    break;
            }
        }

        public async Task NewSceneAsync()
        {
            var page = Application.Current?.Windows.FirstOrDefault()?.Page ?? Application.Current?.MainPage;
            if (page is null)
                return;

            var history = MauiProgram.GetService<ICommandHistoryService>();
            if (history.CanUndo)
            {
                var discard = await page.DisplayAlert(
                    "New Scene",
                    "Create a new scene and discard unsaved editor changes in the current scene?",
                    "Create",
                    "Cancel");
                if (!discard)
                    return;
            }

            var created = await MauiProgram.GetService<WorldService>().CreateNewWorldAsync();
            await DisplayStatus("New Scene", created ? "Created a new untitled scene." : "Unable to create a new scene.");
        }

        public Task UndoAsync()
        {
            return MauiProgram.GetService<ICommandHistoryService>().UndoAsync(new CommandOrigin(CommandOriginKind.UI, "ToolbarUndo"));
        }

        public Task RedoAsync()
        {
            return MauiProgram.GetService<ICommandHistoryService>().RedoAsync(new CommandOrigin(CommandOriginKind.UI, "ToolbarRedo"));
        }

        public async Task RunWorldAsync(
            bool debug,
            CancellationToken cancellationToken = default)
        {
            if (await engineService.GetEditorSimulationStateAsync(
                    cancellationToken))
            {
                await DisplayStatus(
                    debug ? "Debug" : "Play",
                    "Stop simulation before launching the world.");
                return;
            }

            var launchContext = engineService.GetLaunchContext();
            var world =
                await engineService.SerializeCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            Directory.CreateDirectory(launchContext.CacheDirectory);

            await File.WriteAllTextAsync(
                launchContext.TempWorldFilePath,
                world,
                cancellationToken).ConfigureAwait(false);

            engineService.RunWorld(
                launchContext.TempWorldRuntimePath,
                debug,
                launchContext);
        }

        public async Task ToggleSimulationAsync(
            CancellationToken cancellationToken = default)
        {
            await simulationGate.WaitAsync(cancellationToken);
            try
            {
                var nativeState = await engineService
                    .GetEditorSimulationStateAsync(cancellationToken);
                var enable = !nativeState;
                if (enable)
                {
                    var selected = MauiProgram
                        .GetService<SelectionService>()
                        .SelectedItem;
                    if (selected is IInspectorEditable editable &&
                        editable.HasPendingInspectorChanges &&
                        !await editable.CommitInspectorChangesAsync())
                    {
                        await DisplayStatus(
                            "Simulate",
                            "Inspector changes could not be committed.");
                        return;
                    }
                }

                if (!await engineService.SetEditorSimulationAsync(
                        enable,
                        cancellationToken))
                {
                    await DisplayStatus(
                        enable ? "Simulate" : "Stop Simulation",
                        enable
                            ? "Unable to snapshot the current world and start physics simulation."
                            : "Unable to restore the world snapshot.");
                    return;
                }

                SetSimulationState(enable);
                if (!enable &&
                    !await engineService.RefreshCurrentWorldAuthoritativelyAsync(
                        cancellationToken))
                {
                    await DisplayStatus(
                        "Stop Simulation",
                        "The world was restored, but the Editor projection could not be refreshed.");
                    return;
                }

                await DisplayStatus(
                    enable ? "Simulate" : "Stop Simulation",
                    enable
                        ? "Physics simulation started. Scene changes will be restored on Stop."
                        : "Physics simulation stopped and the scene snapshot was restored.");
            }
            finally
            {
                simulationGate.Release();
            }
        }

        public async Task ExportPathTracedImageAsync(bool selectedOnly)
        {
            var engineService = MauiProgram.GetService<EngineService>();
            var selectionService = MauiProgram.GetService<SelectionService>();

            InstanceId targetInstance = null;
            if (selectedOnly)
            {
                var selectedItem = selectionService.SelectedItems.FirstOrDefault();
                switch (selectedItem)
                {
                    case ViewModelComponent component:
                        targetInstance = component.InstanceId;
                        break;

                    case ViewModelGameObject gameObject:
                        targetInstance = gameObject.InstanceId;
                        break;
                }

                if (targetInstance == null || targetInstance.IsEmpty())
                {
                    await DisplayStatus("Path Tracing", "Select a GameObject or a component first.");
                    return;
                }
            }

            var outputDir = Path.Combine(engineService.EngineCacheDirectory, "PathTracing");
            Directory.CreateDirectory(outputDir);

            var mode = selectedOnly ? "selection" : "scene";
            var outputPath = Path.Combine(outputDir, $"pathtrace_{mode}_{DateTime.Now:yyyyMMdd_HHmmss}.png");

            bool exported =
                await engineService.ExportPathTracedImageAsync(
                    outputPath,
                    targetInstance);
            string message = exported ? $"Saved: {outputPath}" : "Path tracing export failed. Check Console panel for details.";

            await DisplayStatus("Path Tracing", message);
        }

        public Task SaveLayoutAsync()
        {
            return MauiProgram.GetService<EditorShellHost>().SaveLayoutAsync();
        }

        public Task ResetLayoutAsync()
        {
            return MauiProgram.GetService<EditorShellHost>().ResetLayoutAsync();
        }

        public async Task OpenSettingsAsync()
        {
            await MauiProgram.GetService<EditorShellHost>().OpenPanelAsync(KnownPanelTypes.Settings);
        }

        static Task DisplayStatus(string title, string message)
        {
            MauiProgram.GetService<EditorShellHost>().SetStatus(message);
            return Task.CompletedTask;
        }

        void SetSimulationState(bool value)
        {
            var next = value ? 1 : 0;
            if (Interlocked.Exchange(ref isSimulating, next) == next)
                return;

            void Publish() => SimulationStateChanged(value);
            if (MainThread.IsMainThread)
                Publish();
            else
                MainThread.BeginInvokeOnMainThread(Publish);
        }
    }
}
