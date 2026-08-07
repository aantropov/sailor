#nullable enable
using SailorEditor.Helpers;
using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.Settings;
using SailorEditor.Controls;
using SailorEditor.Scene;
using SailorEditor.Utility;
using SailorEditor.Workflow;
using SailorEngine;
using System.Diagnostics;
using System.Text.RegularExpressions;

namespace SailorEditor.Views
{
    public partial class SceneView : ContentView
    {
        bool isRunning = false;
        bool isFocused = false;
        bool isInputCaptured = false;
        bool isPlayMode = false;
        bool viewportRetryQueued = false;
        int engineRestartInProgress;
        long viewportRetryUntilMs = -1;
        NativeSceneViewport? nativeViewportHost;
        nint nativeHostHandle = nint.Zero;
        double nativeViewportWidth = 0;
        double nativeViewportHeight = 0;
        double nativeViewportScale = 1;
        uint nativeDrawableWidth = 0;
        uint nativeDrawableHeight = 0;
        double lastRequestedNativeViewportWidth = -1;
        double lastRequestedNativeViewportHeight = -1;
        double lastRequestedNativeViewportScale = -1;
        double lastAppliedLogicalViewportWidth = -1;
        double lastAppliedLogicalViewportHeight = -1;
        double lastAppliedNativeViewportScale = -1;
        uint lastAppliedRenderTargetWidth = 0;
        uint lastAppliedRenderTargetHeight = 0;
        string lastViewportStatusText = string.Empty;
        readonly Stopwatch uiUpdateStopwatch = Stopwatch.StartNew();
        readonly EngineService engineService;
        readonly WorldService worldService;
        readonly ICommandDispatcher commandDispatcher;
        readonly IActionContextProvider actionContextProvider;
        readonly InspectorPendingEditCoordinator inspectorPendingEditCoordinator;
        readonly WorkspaceUiService workspaceUiService;
        readonly IAlreadyAppliedTransformTarget viewportTransformTarget;
        readonly SceneViewportLifecycleAdapter viewportAdapter;
        readonly SceneShellFocusCoordinator focusCoordinator;
        readonly SceneViewportSelectionRouter selectionRouter = new(NullSceneViewportSelectionPicker.Instance);
        readonly EditorViewportEventRevisionGate viewportEventRevisionGate = new();
        readonly UnifiedSettingsStore settingsStore;
        readonly NativeViewportInputQueue nativeViewportInputQueue;
        readonly object viewportToolStateLock = new();
        readonly SemaphoreSlim viewportToolStateGate = new(1, 1);
        SceneViewportToolState viewportToolState =
            SceneViewportToolShortcuts.Default;
        long lastViewportIntegrationTickMs = -1;
        long lastViewportStatusTickMs = -1;
        bool lifecycleSubscribed;
#if MACCATALYST
        static readonly bool UseNativeViewportHost = true;
#else
        static readonly bool UseNativeViewportHost = false;
#endif

        public SceneView()
        {
            InitializeComponent();
            UpdateViewportToolVisuals();
            settingsStore = MauiProgram.GetService<UnifiedSettingsStore>();
            engineService = MauiProgram.GetService<EngineService>();
            worldService = MauiProgram.GetService<WorldService>();
            commandDispatcher = MauiProgram.GetService<ICommandDispatcher>();
            actionContextProvider = MauiProgram.GetService<IActionContextProvider>();
            inspectorPendingEditCoordinator = MauiProgram.GetService<InspectorPendingEditCoordinator>();
            workspaceUiService = MauiProgram.GetService<WorkspaceUiService>();
            nativeViewportInputQueue = new NativeViewportInputQueue(
                ProcessNativeViewportInputAsync,
                exception => Console.WriteLine(
                    $"[SceneView] Native viewport input failed: {exception}"));
            viewportTransformTarget = new EditorViewportTransformTarget(engineService, worldService);
            viewportAdapter = new SceneViewportLifecycleAdapter(new EngineSceneViewportBackend(engineService), EngineService.SceneViewportId);
            UpdateRestartEngineButton(engineService.State);
            var shellState = MauiProgram.GetService<State.ShellState>();
            focusCoordinator = new SceneShellFocusCoordinator(shellState, $"scene:{EngineService.SceneViewportId}", () => ResolveFocusTarget(shellState));

#if !MACCATALYST
            var tapGesture = new TapGestureRecognizer();
            tapGesture.Tapped += (sender, args) =>
            {
                SetSceneFocus(true, sendRemoteFocus: true);
                nativeViewportHost?.RequestInputFocus();
            };
            GestureRecognizers.Add(tapGesture);
#endif

            GestureRecognizers.Add(CreateSceneDropGesture());
            Viewport.GestureRecognizers.Add(CreateSceneDropGesture());
            NativeViewportContainer.GestureRecognizers.Add(CreateSceneDropGesture());

            SizeChanged += (sender, args) =>
            {
                RequestNativeViewportLayout();
                QueueViewportRetry();
            };
            Unloaded += (sender, args) =>
            {
                SetSceneFocus(false, sendRemoteFocus: true);
            };

#if MACCATALYST
            if (UseNativeViewportHost)
            {
                nativeViewportHost = new NativeSceneViewport
                {
                    HorizontalOptions = LayoutOptions.Fill,
                    VerticalOptions = LayoutOptions.Fill
                };
                nativeViewportHost.GestureRecognizers.Add(CreateSceneDropGesture());
                nativeViewportHost.HostHandleChanged += handle =>
                {
                    nativeHostHandle = handle;
                    Console.WriteLine($"[SceneView] native host handle changed: 0x{handle.ToInt64():X}");
                    if (handle != nint.Zero)
                    {
                        QueueViewportRetry();
                    }
                };
                nativeViewportHost.HostLayoutChanged += (width, height, scale, drawableWidth, drawableHeight) =>
                {
                    nativeViewportWidth = width;
                    nativeViewportHeight = height;
                    nativeViewportScale = scale > 0 ? scale : 1;
                    nativeDrawableWidth = drawableWidth;
                    nativeDrawableHeight = drawableHeight;
                    Console.WriteLine($"[SceneView] native host layout: {width:0.##}x{height:0.##} scale={nativeViewportScale:0.##} drawable={nativeDrawableWidth}x{nativeDrawableHeight}");
                    QueueViewportRetry();
                    if (isRunning)
                    {
                        UpdateViewportIntegration();
                    }
                };
                nativeViewportHost.InputReceived +=
                    QueueNativeViewportInput;

                NativeViewportContainer.Content = nativeViewportHost;
                RequestNativeViewportLayout();
            }
#endif

            Loaded += (sender, args) =>
            {
                isRunning = true;
                SubscribeToEngineLifecycle();
                _ = RefreshViewportToolStateAsync();
#if MACCATALYST
                if (!UseNativeViewportHost)
                {
                    UpdateViewportStatus();
                    return;
                }
#endif
                RequestNativeViewportLayout();

                Dispatcher.StartTimer(TimeSpan.FromMilliseconds(50), () =>
                {
                    try
                    {
                        if (!isRunning)
                        {
                            return false;
                        }

                        var nowMs = uiUpdateStopwatch.ElapsedMilliseconds;
                        if (lastViewportIntegrationTickMs < 0 || nowMs - lastViewportIntegrationTickMs >= 50)
                        {
                            lastViewportIntegrationTickMs = nowMs;
                            UpdateViewportIntegration();
                        }

                        if (lastViewportStatusTickMs < 0 || nowMs - lastViewportStatusTickMs >= 500)
                        {
                            lastViewportStatusTickMs = nowMs;
                            UpdateViewportStatus();
                        }
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"[SceneView] viewport timer exception: {ex}");
                    }

                    return true;
                });
            };

            Unloaded += (sender, args) =>
            {
                isRunning = false;
                ResetNativeViewportInputQueue();
                UnsubscribeFromEngineLifecycle();
                focusCoordinator.ReleaseIfOwned();
                viewportAdapter.Destroy();
            };
        }

        void QueueNativeViewportInput(
            NativeSceneViewportInputEvent input)
        {
            if (!isRunning)
            {
                return;
            }

            nativeViewportInputQueue.Enqueue(input);
        }

        void ResetNativeViewportInputQueue()
        {
            nativeViewportInputQueue.Reset();
            isInputCaptured = false;
        }

        async Task<NativeViewportInputDispatchResult> ProcessNativeViewportInputAsync(
            NativeSceneViewportInputEvent input,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var captured = input.Captured || isInputCaptured;

            if (input.Kind == NativeSceneViewportInputKind.Key &&
                SceneViewportToolShortcuts.TryApply(
                    input.KeyCode,
                    input.Pressed,
                    isFocused,
                    GetViewportToolState(),
                    out var shortcutState))
            {
                await ApplyViewportToolStateAsync(
                    shortcutState,
                    cancellationToken);
            }

            if (input.Kind ==
                    NativeSceneViewportInputKind.PointerButton &&
                input.Button == 0 &&
                input.Pressed)
            {
                var committed = await inspectorPendingEditCoordinator
                    .CommitPendingChangesAsync(cancellationToken);
                cancellationToken.ThrowIfCancellationRequested();
                if (!committed)
                {
                    Console.WriteLine(
                        "[SceneView] Ignored primary pointer gesture because pending Inspector changes could not be committed.");
                    return NativeViewportInputDispatchResult
                        .RejectPrimaryPointerGesture;
                }
            }

            if (input.Kind == NativeSceneViewportInputKind.Focus)
            {
                SetSceneFocus(
                    input.Focused,
                    sendRemoteFocus: false);
                if (!input.Focused)
                {
                    isInputCaptured = false;
                }
            }
            else if (input.Kind ==
                NativeSceneViewportInputKind.PointerButton)
            {
                if (input.Pressed)
                {
                    SetSceneFocus(
                        true,
                        sendRemoteFocus: !isFocused);
                    nativeViewportHost?.RequestInputFocus();
                }
                isInputCaptured = input.Captured;
                captured = input.Captured;
            }
            else if (input.Kind == NativeSceneViewportInputKind.Key)
            {
                SetSceneFocus(true, sendRemoteFocus: !isFocused);
            }
            else if (!isFocused && !isInputCaptured)
            {
                return NativeViewportInputDispatchResult.Forwarded;
            }

            if (input.Kind == NativeSceneViewportInputKind.Capture)
            {
                isInputCaptured = input.Captured;
                captured = input.Captured;
            }

            if (!isFocused &&
                !captured &&
                input.Kind != NativeSceneViewportInputKind.Focus)
            {
                return NativeViewportInputDispatchResult.Forwarded;
            }

            cancellationToken.ThrowIfCancellationRequested();
            var remoteModifiers =
                (RemoteViewportInputModifier)input.Modifiers;
            var focused =
                input.Kind == NativeSceneViewportInputKind.Focus
                    ? input.Focused
                    : isFocused;
            viewportAdapter.SendInput(
                (RemoteViewportInputKind)input.Kind,
                input.PointerX,
                input.PointerY,
                input.WheelDeltaX,
                input.WheelDeltaY,
                input.KeyCode,
                input.Button,
                remoteModifiers,
                input.Pressed,
                focused,
                captured);
            return NativeViewportInputDispatchResult.Forwarded;
        }

        void SubscribeToEngineLifecycle()
        {
            if (lifecycleSubscribed)
                return;

            engineService.OnLifecycleStateChanged += OnEngineLifecycleStateChanged;
            engineService.OnEditorViewportEvents += OnEditorViewportEvents;
            viewportEventRevisionGate.Reset();
            lifecycleSubscribed = true;
        }

        void UnsubscribeFromEngineLifecycle()
        {
            if (!lifecycleSubscribed)
                return;

            engineService.OnLifecycleStateChanged -= OnEngineLifecycleStateChanged;
            engineService.OnEditorViewportEvents -= OnEditorViewportEvents;
            lifecycleSubscribed = false;
        }

        void OnEngineLifecycleStateChanged(EngineLifecycleState state)
        {
            UpdateRestartEngineButton(state);
            viewportEventRevisionGate.Reset();
            ResetNativeViewportInputQueue();
            if (state != EngineLifecycleState.Running || !isRunning)
                return;

            viewportAdapter.ResetForBackendRestart();
            QueueViewportRetry(TimeSpan.FromSeconds(1));
            UpdateViewportIntegration();
            _ = RefreshViewportToolStateAsync();
        }

        async void OnRestartEngineClicked(object sender, EventArgs e)
        {
            if (Interlocked.Exchange(ref engineRestartInProgress, 1) != 0)
            {
                return;
            }

            UpdateRestartEngineButton(engineService.State);
            try
            {
                await workspaceUiService.RestartEngineAsync();
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine(
                    $"Restart Engine Process failed: {exception}");
            }
            finally
            {
                Interlocked.Exchange(ref engineRestartInProgress, 0);
                UpdateRestartEngineButton(engineService.State);
            }
        }

        void UpdateRestartEngineButton(EngineLifecycleState state)
        {
            var isRestarting = Volatile.Read(ref engineRestartInProgress) != 0;
            RestartEngineButton.IsEnabled =
                !isRestarting &&
                state is EngineLifecycleState.Running or
                    EngineLifecycleState.Stopped or
                    EngineLifecycleState.Faulted;
            RestartEngineButton.Opacity = RestartEngineButton.IsEnabled
                ? 1.0
                : 0.45;
            RestartEngineButton.BackgroundColor = state == EngineLifecycleState.Faulted
                ? Color.FromArgb("#5A2C2C")
                : Colors.Transparent;
        }

        async void OnEditorViewportEvents(IReadOnlyList<EditorViewportEvent> viewportEvents)
        {
            if (!isRunning)
            {
                return;
            }

            foreach (var viewportEvent in viewportEvents)
            {
                if (!viewportEventRevisionGate.TryAccept(viewportEvent))
                {
                    continue;
                }

                try
                {
                    switch (viewportEvent)
                    {
                        case EditorViewportSelectionEvent selectionEvent:
                            await ApplyViewportSelectionAsync(selectionEvent);
                            break;

                        case EditorViewportTransformEvent transformEvent:
                            await ApplyViewportTransformAsync(transformEvent);
                            break;

                        case EditorViewportAssetDropEvent assetDropEvent:
                            await ApplyViewportAssetDropAsync(assetDropEvent);
                            break;

                        case EditorViewportToolShortcutEvent shortcutEvent:
                            await ApplyViewportToolShortcutAsync(shortcutEvent);
                            break;
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[SceneView] Failed to apply viewport event revision {viewportEvent.Revision}: {ex}");
                }
            }
        }

        async Task ApplyViewportSelectionAsync(EditorViewportSelectionEvent viewportEvent)
        {
            var isManagedRevisionCurrent =
                await engineService.IsEditorViewportSelectionEventCurrentAsync(
                    viewportEvent.ManagedMutationRevision);
            if (!isManagedRevisionCurrent ||
                !viewportEventRevisionGate.IsCurrent(viewportEvent))
            {
                Console.WriteLine($"[SceneView] Rejected stale viewport selection revision {viewportEvent.Revision} after a newer managed mutation.");
                return;
            }

            if (!await inspectorPendingEditCoordinator
                    .CommitPendingChangesAsync())
            {
                Console.WriteLine($"[SceneView] Rejected viewport selection revision {viewportEvent.Revision}: pending Inspector changes could not be committed.");
                return;
            }

            isManagedRevisionCurrent =
                await engineService.IsEditorViewportSelectionEventCurrentAsync(
                    viewportEvent.ManagedMutationRevision);
            if (!isManagedRevisionCurrent ||
                !viewportEventRevisionGate.IsCurrent(viewportEvent))
            {
                Console.WriteLine($"[SceneView] Rejected viewport selection revision {viewportEvent.Revision} after committing pending Inspector changes.");
                return;
            }

            InstanceId? selectedId = null;
            if (!string.IsNullOrEmpty(viewportEvent.SelectedInstanceId))
            {
                var candidate = new InstanceId(viewportEvent.SelectedInstanceId);
                if (candidate.IsEmpty() || !worldService.TryGetGameObject(candidate, out _))
                {
                    Console.WriteLine($"[SceneView] Rejected viewport selection revision {viewportEvent.Revision}: '{viewportEvent.SelectedInstanceId}' is not a live GameObject.");
                    return;
                }

                selectedId = candidate;
            }

            var result = await commandDispatcher.DispatchAsync(
                new ApplyRuntimeSelectionCommand(selectedId),
                CreateViewportActionContext(viewportEvent));
            if (!result.Succeeded)
            {
                Console.WriteLine($"[SceneView] Viewport selection revision {viewportEvent.Revision} failed: {result.Message}");
            }
        }

        async Task ApplyViewportTransformAsync(EditorViewportTransformEvent viewportEvent)
        {
            var isManagedRevisionCurrent =
                await engineService.IsEditorViewportTransformEventCurrentAsync(
                    viewportEvent.InstanceId,
                    viewportEvent.ManagedMutationRevision);
            if (!isManagedRevisionCurrent ||
                !viewportEventRevisionGate.IsCurrent(viewportEvent))
            {
                Console.WriteLine($"[SceneView] Rejected stale viewport transform revision {viewportEvent.Revision} after a newer managed mutation.");
                return;
            }

            if (!await inspectorPendingEditCoordinator
                    .CommitPendingChangesAsync())
            {
                Console.WriteLine($"[SceneView] Rejected viewport transform revision {viewportEvent.Revision}: pending Inspector changes could not be committed.");
                return;
            }

            isManagedRevisionCurrent =
                await engineService.IsEditorViewportTransformEventCurrentAsync(
                    viewportEvent.InstanceId,
                    viewportEvent.ManagedMutationRevision);
            if (!isManagedRevisionCurrent ||
                !viewportEventRevisionGate.IsCurrent(viewportEvent))
            {
                Console.WriteLine($"[SceneView] Rejected viewport transform revision {viewportEvent.Revision} after committing pending Inspector changes.");
                return;
            }

            var instanceId = new InstanceId(viewportEvent.InstanceId);
            if (instanceId.IsEmpty() || !worldService.TryGetGameObject(instanceId, out var gameObject))
            {
                Console.WriteLine($"[SceneView] Rejected viewport transform revision {viewportEvent.Revision}: '{viewportEvent.InstanceId}' is not a live GameObject.");
                return;
            }

            var structure = CaptureGameObjectStructure(gameObject);
            var beforeYaml = EditorYaml.SerializeGameObject(CreateTransformSnapshot(
                structure,
                viewportEvent.BeforePosition,
                viewportEvent.BeforeRotation,
                viewportEvent.BeforeScale));
            var afterYaml = EditorYaml.SerializeGameObject(CreateTransformSnapshot(
                structure,
                viewportEvent.AfterPosition,
                viewportEvent.AfterRotation,
                viewportEvent.AfterScale));
            if (string.Equals(beforeYaml, afterYaml, StringComparison.Ordinal))
            {
                return;
            }

            var result = await commandDispatcher.DispatchAsync(
                new AlreadyAppliedTransformCommand(
                    viewportEvent.InstanceId,
                    beforeYaml,
                    afterYaml,
                    viewportTransformTarget,
                    $"{viewportEvent.Operation} {gameObject.Name}"),
                CreateViewportActionContext(viewportEvent));
            if (!result.Succeeded)
            {
                Console.WriteLine($"[SceneView] Viewport transform revision {viewportEvent.Revision} failed: {result.Message}");
            }
        }

        async Task ApplyViewportAssetDropAsync(
            EditorViewportAssetDropEvent viewportEvent)
        {
            var fileId = new FileId(viewportEvent.FileId);
            if (!MauiProgram.GetService<AssetsService>()
                    .Assets.TryGetValue(fileId, out var assetFile))
            {
                Console.WriteLine(
                    $"[SceneView] Rejected viewport asset drop revision {viewportEvent.Revision}: '{viewportEvent.FileId}' is not a known asset.");
                return;
            }

            var result = await DispatchViewportAssetDropAsync(
                assetFile,
                viewportEvent.NormalizedX,
                viewportEvent.NormalizedY,
                CreateViewportActionContext(viewportEvent));
            if (!result.Succeeded)
            {
                Console.WriteLine(
                    $"[SceneView] Viewport asset drop revision {viewportEvent.Revision} failed: {result.Message}");
            }
        }

        async Task ApplyViewportToolShortcutAsync(
            EditorViewportToolShortcutEvent viewportEvent)
        {
            SetSceneFocus(true, sendRemoteFocus: !isFocused);
            if (!SceneViewportToolShortcuts.TryApply(
                    viewportEvent.KeyCode,
                    isPressed: true,
                    hasViewportFocus: true,
                    GetViewportToolState(),
                    out var shortcutState))
            {
                Console.WriteLine(
                    $"[SceneView] Rejected viewport tool shortcut revision {viewportEvent.Revision}: key={viewportEvent.KeyCode}.");
                return;
            }

            lock (viewportToolStateLock)
            {
                viewportToolState = shortcutState;
            }
            UpdateViewportToolVisuals();
            await ApplyViewportToolStateSafelyAsync(shortcutState);
        }

        ActionContext CreateViewportActionContext(EditorViewportEvent viewportEvent) =>
            actionContextProvider.GetCurrentContext(
                new CommandOrigin(CommandOriginKind.UI, nameof(SceneView)),
                new Dictionary<string, string?>
                {
                    ["viewportEventRevision"] = viewportEvent.Revision.ToString(System.Globalization.CultureInfo.InvariantCulture),
                });

        static Vec4 ToVec4(EditorViewportVector4 value) => new()
        {
            X = value.X,
            Y = value.Y,
            Z = value.Z,
            W = value.W,
        };

        static GameObjectStructure CaptureGameObjectStructure(SailorEditor.ViewModels.GameObject gameObject) => new(
            gameObject.Name,
            gameObject.InstanceId.Value,
            gameObject.ParentIndex,
            new List<int>(gameObject.ComponentIndices ?? []));

        static SailorEditor.ViewModels.GameObject CreateTransformSnapshot(
            GameObjectStructure structure,
            EditorViewportVector4 position,
            EditorViewportVector4 rotation,
            EditorViewportVector4 scale) => new()
        {
            Name = structure.Name,
            InstanceId = new InstanceId(structure.InstanceId),
            ParentIndex = structure.ParentIndex,
            ComponentIndices = new List<int>(structure.ComponentIndices),
            Position = ToVec4(position),
            Rotation = new Rotation(new Quat(rotation.X, rotation.Y, rotation.Z, rotation.W)),
            Scale = ToVec4(scale),
        };

        readonly record struct GameObjectStructure(
            string Name,
            string InstanceId,
            uint ParentIndex,
            IReadOnlyList<int> ComponentIndices);

        SceneShellFocusTarget ResolveFocusTarget(State.ShellState shellState)
        {
            var panel = shellState.OpenPanels.FirstOrDefault(x => ReferenceEquals(x.View, this));
            return panel is null
                ? default
                : new SceneShellFocusTarget(panel.PanelId, panel.GroupId, panel.Role == Panels.PanelRole.Document ? panel.PanelId : shellState.Focus.ActiveDocumentPanelId);
        }

        async void OnSceneDrop(object? sender, DropEventArgs e)
            => await ExecuteSceneUiActionAsync(
                () => HandleSceneDropAsync(e),
                "Viewport drop");

        async Task HandleSceneDropAsync(DropEventArgs e)
        {
            if (e.Handled || !e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var source))
            {
                return;
            }

            var context = actionContextProvider.GetCurrentContext(
                new CommandOrigin(
                    CommandOriginKind.DragDrop,
                    nameof(SceneView)));
            if (EditorDragDrop.IsViewportAssetDrop(source))
            {
                e.Handled = true;
                var pointer = e.GetPosition(NativeViewportContainer) ??
                    e.GetPosition(Viewport);
                if (pointer is null ||
                    !SceneViewportDropCoordinateResolver.TryResolve(
                        pointer.Value.X,
                        pointer.Value.Y,
                        NativeViewportContainer.Width,
                        NativeViewportContainer.Height,
                        out var coordinates))
                {
                    return;
                }

                var result = await DispatchViewportAssetDropAsync(
                    source,
                    coordinates.NormalizedX,
                    coordinates.NormalizedY,
                    context);
                if (!result.Succeeded)
                {
                    Console.WriteLine(
                        $"[SceneView] Viewport drop failed: {result.Message}");
                }
                return;
            }

            if (EditorDragDrop.TryCreateSceneDropCommand(
                    source,
                    null,
                    out var command) &&
                command is not null)
            {
                e.Handled = true;
                await commandDispatcher.DispatchAsync(command, context);
            }
        }

        async Task<CommandResult> DispatchViewportAssetDropAsync(
            object source,
            double normalizedX,
            double normalizedY,
            ActionContext context)
        {
            var worldPosition = await engineService
                .TraceViewportRayAsync(
                    normalizedX,
                    normalizedY);
            if (worldPosition is null)
            {
                return CommandResult.Failure(
                    "Viewport ray target could not be resolved");
            }

            if (!EditorDragDrop.TryCreateViewportDropCommand(
                    source,
                    worldPosition,
                    out var command) ||
                command is null)
            {
                return CommandResult.Failure(
                    "The dropped asset is not a model or prefab");
            }

            return await commandDispatcher.DispatchAsync(
                command,
                context);
        }

        DropGestureRecognizer CreateSceneDropGesture()
        {
            var gesture = new DropGestureRecognizer();
            gesture.DragOver += (_, e) =>
            {
                if (e.Data.Properties.TryGetValue(
                        EditorDragDrop.DragItemKey,
                        out var source) &&
                    (EditorDragDrop.IsViewportAssetDrop(source) ||
                     source is SailorEditor.ViewModels.GameObject))
                {
                    e.AcceptedOperation = DataPackageOperation.Copy;
                }
            };
            gesture.Drop += OnSceneDrop;
            return gesture;
        }

        void OnSelectToolClicked(object sender, EventArgs e) =>
            QueueViewportToolState(
                GetViewportToolState() with
                {
                    Operation =
                        EditorViewportTransformOperation.Select
                });

        void OnTranslateToolClicked(object sender, EventArgs e) =>
            QueueViewportToolState(
                GetViewportToolState() with
                {
                    Operation =
                        EditorViewportTransformOperation.Translate
                });

        void OnRotateToolClicked(object sender, EventArgs e) =>
            QueueViewportToolState(
                GetViewportToolState() with
                {
                    Operation =
                        EditorViewportTransformOperation.Rotate
                });

        void OnScaleToolClicked(object sender, EventArgs e) =>
            QueueViewportToolState(
                GetViewportToolState() with
                {
                    Operation =
                        EditorViewportTransformOperation.Scale
                });

        void OnTransformSpaceClicked(object sender, EventArgs e)
        {
            var current = GetViewportToolState();
            QueueViewportToolState(
                current with
                {
                    Space =
                        current.Space ==
                        EditorViewportTransformSpace.Local
                            ? EditorViewportTransformSpace.World
                            : EditorViewportTransformSpace.Local
                });
        }

        void QueueViewportToolState(SceneViewportToolState state)
        {
            SetSceneFocus(true, sendRemoteFocus: !isFocused);
            nativeViewportHost?.RequestInputFocus();
            lock (viewportToolStateLock)
            {
                viewportToolState = state;
            }
            UpdateViewportToolVisuals();
            _ = ApplyViewportToolStateSafelyAsync(state);
        }

        async Task ApplyViewportToolStateSafelyAsync(
            SceneViewportToolState state)
        {
            try
            {
                if (!await ApplyViewportToolStateAsync(
                        state,
                        CancellationToken.None))
                {
                    Console.WriteLine(
                        "[SceneView] Viewport tool state was rejected by the engine.");
                    await RefreshViewportToolStateAsync();
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine(
                    $"[SceneView] Failed to set viewport tool state: {ex}");
                await RefreshViewportToolStateAsync();
            }
        }

        async Task<bool> ApplyViewportToolStateAsync(
            SceneViewportToolState state,
            CancellationToken cancellationToken)
        {
            await viewportToolStateGate.WaitAsync(cancellationToken);
            try
            {
                if (!await engineService.SetViewportToolStateAsync(
                        state.Operation,
                        state.Space,
                        cancellationToken))
                {
                    return false;
                }

                lock (viewportToolStateLock)
                {
                    viewportToolState = state;
                }

                Dispatcher.Dispatch(UpdateViewportToolVisuals);
                return true;
            }
            finally
            {
                viewportToolStateGate.Release();
            }
        }

        async Task RefreshViewportToolStateAsync()
        {
            try
            {
                await viewportToolStateGate.WaitAsync();
                try
                {
                    var state = await engineService
                        .GetViewportToolStateAsync();
                    if (state is null)
                    {
                        return;
                    }

                    lock (viewportToolStateLock)
                    {
                        viewportToolState = state.Value;
                    }

                    Dispatcher.Dispatch(UpdateViewportToolVisuals);
                }
                finally
                {
                    viewportToolStateGate.Release();
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine(
                    $"[SceneView] Failed to read viewport tool state: {ex}");
            }
        }

        SceneViewportToolState GetViewportToolState()
        {
            lock (viewportToolStateLock)
            {
                return viewportToolState;
            }
        }

        void UpdateViewportToolVisuals()
        {
            var state = GetViewportToolState();
            var activeColor = Color.FromArgb("#455F80");
            var inactiveColor = Color.FromArgb("#303030");
            SelectToolButton.BackgroundColor =
                state.Operation ==
                EditorViewportTransformOperation.Select
                    ? activeColor
                    : inactiveColor;
            TranslateToolButton.BackgroundColor =
                state.Operation ==
                EditorViewportTransformOperation.Translate
                    ? activeColor
                    : inactiveColor;
            RotateToolButton.BackgroundColor =
                state.Operation ==
                EditorViewportTransformOperation.Rotate
                    ? activeColor
                    : inactiveColor;
            ScaleToolButton.BackgroundColor =
                state.Operation ==
                EditorViewportTransformOperation.Scale
                    ? activeColor
                    : inactiveColor;
            TransformSpaceButton.BackgroundColor = inactiveColor;
            TransformSpaceButton.Text =
                state.Space == EditorViewportTransformSpace.Local
                    ? "T  Local"
                    : "T  World";
        }

        static async Task ExecuteSceneUiActionAsync(
            Func<Task> action,
            string operation)
        {
            try
            {
                await action();
            }
            catch (Exception ex)
            {
                Console.WriteLine(
                    $"[SceneView] {operation} failed: {ex}");
            }
        }

        void SetSceneFocus(bool focused, bool sendRemoteFocus)
        {
            if (isFocused == focused)
            {
                if (sendRemoteFocus)
                    viewportAdapter.SendInput(RemoteViewportInputKind.Focus, focused: focused);
                UpdateFocusVisual();
                return;
            }

            isFocused = focused;
            focusCoordinator.SetViewportFocus(focused);
            if (sendRemoteFocus)
                viewportAdapter.SendInput(RemoteViewportInputKind.Focus, focused: focused);
            UpdateFocusVisual();
            UpdateViewportIntegration();
        }

        void UpdateFocusVisual()
        {
            if (isPlayMode)
            {
                Viewport.Stroke = Color.FromArgb("#C84747");
            }
            else if (isFocused)
            {
                Viewport.Stroke = Color.FromArgb("#8A8A8A");
            }
            else
            {
                Viewport.Stroke = Color.FromArgb("#1A1A1A");
            }
        }

        void UpdateViewportIntegration()
        {
            if (nativeViewportHost != null)
            {
                nativeViewportHost.MouseSensitivity = ResolveViewportMouseSensitivity();
            }

            var remoteRect = Viewport.GetAbsolutePositionWin();
            var editorRect = new SceneViewportRect(remoteRect.X, remoteRect.Y, remoteRect.Width, remoteRect.Height);
            var renderTarget = default(SceneViewportRenderTarget);
#if MACCATALYST
            if (UseNativeViewportHost)
            {
                if (nativeHostHandle == nint.Zero)
                {
                    if (ViewportStatusOverlay.IsVisible)
                        SetLabelText(ViewportStatusText, "Waiting for native CAMetalLayer host…");
                    return;
                }

                var logicalViewportWidth = nativeViewportWidth > 1 ? nativeViewportWidth : NativeViewportContainer.Width;
                var logicalViewportHeight = nativeViewportHeight > 1 ? nativeViewportHeight : NativeViewportContainer.Height;
                if (logicalViewportWidth <= 1 || logicalViewportHeight <= 1)
                {
                    if (ViewportStatusOverlay.IsVisible)
                        SetLabelText(ViewportStatusText, $"Waiting for native viewport layout… host={nativeHostHandle != nint.Zero} size={logicalViewportWidth:0.##}x{logicalViewportHeight:0.##}");
                    return;
                }

                var renderWidth = nativeDrawableWidth > 1 ? nativeDrawableWidth : (uint)Math.Max(1, Math.Round(logicalViewportWidth * nativeViewportScale));
                var renderHeight = nativeDrawableHeight > 1 ? nativeDrawableHeight : (uint)Math.Max(1, Math.Round(logicalViewportHeight * nativeViewportScale));
                if (Math.Abs(lastAppliedLogicalViewportWidth - logicalViewportWidth) >= 0.5 ||
                    Math.Abs(lastAppliedLogicalViewportHeight - logicalViewportHeight) >= 0.5 ||
                    Math.Abs(lastAppliedNativeViewportScale - nativeViewportScale) >= 0.01 ||
                    lastAppliedRenderTargetWidth != renderWidth ||
                    lastAppliedRenderTargetHeight != renderHeight)
                {
                    lastAppliedLogicalViewportWidth = logicalViewportWidth;
                    lastAppliedLogicalViewportHeight = logicalViewportHeight;
                    lastAppliedNativeViewportScale = nativeViewportScale;
                    lastAppliedRenderTargetWidth = renderWidth;
                    lastAppliedRenderTargetHeight = renderHeight;
                    Console.WriteLine($"[SceneView] viewport sizes: logical={logicalViewportWidth:0.##}x{logicalViewportHeight:0.##} scale={nativeViewportScale:0.##} render={renderWidth}x{renderHeight}");
                    QueueViewportRetry(TimeSpan.FromMilliseconds(350));
                }

                editorRect = new SceneViewportRect(0, 0, Math.Max(1, logicalViewportWidth), Math.Max(1, logicalViewportHeight));
                remoteRect = new Rect(0, 0, renderWidth, renderHeight);
                renderTarget = new SceneViewportRenderTarget(renderWidth, renderHeight);
            }
#endif

            if (remoteRect.IsEmpty)
            {
                if (ViewportStatusOverlay.IsVisible)
                    SetLabelText(ViewportStatusText, "Waiting for viewport host…");
                return;
            }

            var updated = viewportAdapter.Sync(new SceneViewportFrame(
                new SceneViewportRect(remoteRect.X, remoteRect.Y, remoteRect.Width, remoteRect.Height),
                editorRect,
                renderTarget,
                IsVisible,
                isFocused,
                nativeHostHandle));

            if (!updated)
                viewportAdapter.Sync(new SceneViewportFrame(editorRect, editorRect, renderTarget, IsVisible, isFocused, nativeHostHandle));
        }

        double ResolveViewportMouseSensitivity()
        {
            var value = settingsStore.Resolve(EditorSettingsCatalog.EditorViewportMouseSensitivity.Entry).Value;
            return value switch
            {
                double d => d,
                float f => f,
                int i => i,
                long l => l,
                _ => 1.0
            };
        }

        void UpdateViewportStatus()
        {
            var state = viewportAdapter.GetState();
            var diagnostics = viewportAdapter.GetDiagnostics();

#if MACCATALYST
            if (!UseNativeViewportHost)
            {
                SetLabelText(ViewportStatusText, "MacCatalyst native viewport host temporarily disabled to avoid startup crash");
                SetLabelText(ViewportTruthText, "Truth: native host disabled");
                ViewportTruthText.TextColor = Color.FromArgb("#FF6B6B");
                SetLabelText(ViewportSourceText, "Source: unavailable");
                SetLabelText(ViewportEvidenceText, "Evidence: unavailable");
                SetLabelText(ViewportGeometryText, "Viewport: unavailable");
                SetLabelText(ViewportDiagnosticsText, diagnostics);
                RetryRemoteViewportButton.IsVisible = false;
                return;
            }
#endif

            SetLabelText(ViewportStatusText, SceneViewportStatusText.Describe(state));

            if (!string.Equals(lastViewportStatusText, ViewportStatusText.Text, StringComparison.Ordinal))
            {
                lastViewportStatusText = ViewportStatusText.Text;
                Console.WriteLine($"[SceneView] status='{ViewportStatusText.Text}' diagnostics='{diagnostics}'");
            }

#if MACCATALYST
            if (!ViewportStatusOverlay.IsVisible)
            {
                RetryRemoteViewportButton.IsVisible = false;
                return;
            }
#endif

            var parsed = ParseViewportDiagnostics(diagnostics);
            ApplyViewportDiagnosticSummary(parsed, state, diagnostics);
            RetryRemoteViewportButton.IsVisible = state is RemoteViewportSessionState.Recovering or RemoteViewportSessionState.Lost;
        }

        void RequestNativeViewportLayout()
        {
#if MACCATALYST
            if (!UseNativeViewportHost || nativeViewportHost is null)
            {
                return;
            }

            var width = NativeViewportContainer.Width > 1 ? NativeViewportContainer.Width : Viewport.Width;
            var height = NativeViewportContainer.Height > 1 ? NativeViewportContainer.Height : Viewport.Height;
            if (width <= 1 || height <= 1)
            {
                return;
            }

            var scale = nativeViewportScale > 0 ? nativeViewportScale : 1;
            if (Math.Abs(lastRequestedNativeViewportWidth - width) < 0.5 &&
                Math.Abs(lastRequestedNativeViewportHeight - height) < 0.5 &&
                Math.Abs(lastRequestedNativeViewportScale - scale) < 0.01)
            {
                return;
            }

            lastRequestedNativeViewportWidth = width;
            lastRequestedNativeViewportHeight = height;
            lastRequestedNativeViewportScale = scale;
            nativeViewportHost.RequestLayoutUpdate(width, height, scale);
#endif
        }

        void QueueViewportRetry(TimeSpan? keepAlive = null)
        {
#if MACCATALYST
            if (!UseNativeViewportHost || !isRunning)
            {
                return;
            }

            if (keepAlive.HasValue)
            {
                viewportRetryUntilMs = Math.Max(viewportRetryUntilMs, uiUpdateStopwatch.ElapsedMilliseconds + (long)keepAlive.Value.TotalMilliseconds);
            }

            if (viewportRetryQueued)
            {
                return;
            }

            viewportRetryQueued = true;
            Dispatcher.DispatchDelayed(TimeSpan.FromMilliseconds(33), () =>
            {
                viewportRetryQueued = false;
                if (!isRunning || nativeHostHandle == nint.Zero)
                {
                    return;
                }

                var width = NativeViewportContainer.Width > 1 ? NativeViewportContainer.Width : Viewport.Width;
                var height = NativeViewportContainer.Height > 1 ? NativeViewportContainer.Height : Viewport.Height;
                if (width <= 1 || height <= 1)
                {
                    return;
                }

                nativeViewportHost?.RequestLayoutUpdate(width, height, nativeViewportScale);
                UpdateViewportIntegration();
                viewportAdapter.Retry();

                if (uiUpdateStopwatch.ElapsedMilliseconds < viewportRetryUntilMs)
                {
                    QueueViewportRetry();
                }
            });
#endif
        }

        void OnRetryRemoteViewportClicked(object sender, EventArgs e)
        {
            viewportAdapter.Retry();
        }

        sealed class ParsedViewportDiagnostics
        {
            public string SourceName { get; set; } = "unknown";
            public string SourceKind { get; set; } = "unknown";
            public bool? SyntheticSource { get; set; }
            public bool? NativeLayer { get; set; }
            public bool? Readable { get; set; }
            public bool? NonBlack { get; set; }
            public bool? Variance { get; set; }
            public string Size { get; set; } = "unknown";
            public string SourceSize { get; set; } = "unknown";
            public string SourcePitch { get; set; } = "unknown";
            public string PresentCount { get; set; } = "0";
            public string LastGoodFrame { get; set; } = "0";
            public string Event { get; set; } = string.Empty;
            public string DrawableToken { get; set; } = "0";
            public string CopyToken { get; set; } = "0";
            public string AvgLuma { get; set; } = "?";
            public string MaxLuma { get; set; } = "?";
            public string NonBlackPct { get; set; } = "?";
            public string Checksum { get; set; } = "?";
            public string TopLeftSample { get; set; } = "?";
            public string CenterSample { get; set; } = "?";
            public string BottomRightSample { get; set; } = "?";
            public string ProbeSummary { get; set; } = string.Empty;
        }

        static ParsedViewportDiagnostics ParseViewportDiagnostics(string diagnostics)
        {
            var parsed = new ParsedViewportDiagnostics();
            if (string.IsNullOrWhiteSpace(diagnostics))
            {
                return parsed;
            }

            parsed.SourceName = MatchValue(diagnostics, "sourceName") ?? parsed.SourceName;
            parsed.SourceKind = MatchValue(diagnostics, "sourceKind") ?? parsed.SourceKind;
            parsed.SyntheticSource = MatchBool(diagnostics, "syntheticSource");
            parsed.NativeLayer = MatchBool(diagnostics, "nativeLayer");
            parsed.Readable = MatchBool(diagnostics, "readable");
            parsed.NonBlack = MatchBool(diagnostics, "nonBlack");
            parsed.Variance = MatchBool(diagnostics, "variance");
            parsed.Size = MatchValue(diagnostics, "size") ?? parsed.Size;
            parsed.SourceSize = MatchValue(diagnostics, "srcSize") ?? parsed.SourceSize;
            parsed.SourcePitch = MatchValue(diagnostics, "srcPitch") ?? parsed.SourcePitch;
            parsed.PresentCount = MatchValue(diagnostics, "presentCount") ?? parsed.PresentCount;
            parsed.LastGoodFrame = MatchValue(diagnostics, "lastGoodFrame") ?? parsed.LastGoodFrame;
            parsed.Event = MatchValue(diagnostics, "event") ?? string.Empty;
            parsed.DrawableToken = MatchValue(diagnostics, "drawableToken") ?? parsed.DrawableToken;
            parsed.CopyToken = MatchValue(diagnostics, "copyToken") ?? parsed.CopyToken;
            parsed.AvgLuma = MatchValue(diagnostics, "avgLuma") ?? parsed.AvgLuma;
            parsed.MaxLuma = MatchValue(diagnostics, "maxLuma") ?? parsed.MaxLuma;
            parsed.NonBlackPct = MatchValue(diagnostics, "nonBlackPct") ?? parsed.NonBlackPct;
            parsed.Checksum = MatchValue(diagnostics, "checksum") ?? parsed.Checksum;
            parsed.TopLeftSample = MatchValue(diagnostics, "tl") ?? parsed.TopLeftSample;
            parsed.CenterSample = MatchValue(diagnostics, "c") ?? parsed.CenterSample;
            parsed.BottomRightSample = MatchValue(diagnostics, "br") ?? parsed.BottomRightSample;
            parsed.ProbeSummary = MatchProbeSummary(diagnostics);
            return parsed;
        }

        void ApplyViewportDiagnosticSummary(ParsedViewportDiagnostics parsed, RemoteViewportSessionState state, string diagnostics)
        {
            var seesRealSource = parsed.SyntheticSource == false;
            var seesSceneEvidence = parsed.Readable == true && (parsed.NonBlack == true || parsed.Variance == true);
            var onlyBlackEvidence = parsed.Readable == true && parsed.NonBlack == false && parsed.Variance == false;

            if (state == RemoteViewportSessionState.Active && seesRealSource && seesSceneEvidence)
            {
                SetLabelText(ViewportTruthText, "Truth: REAL renderer source with meaningful scene pixels");
                ViewportTruthText.TextColor = Color.FromArgb("#7DFFB3");
            }
            else if (state == RemoteViewportSessionState.Active && parsed.SyntheticSource == true)
            {
                SetLabelText(ViewportTruthText, "Truth: ACTIVE but still showing synthetic source");
                ViewportTruthText.TextColor = Color.FromArgb("#FFB86B");
            }
            else if (state == RemoteViewportSessionState.Active && onlyBlackEvidence)
            {
                SetLabelText(ViewportTruthText, "Truth: ACTIVE but frame samples still read as black/static");
                ViewportTruthText.TextColor = Color.FromArgb("#FFD166");
            }
            else
            {
                SetLabelText(ViewportTruthText, "Truth: not enough proof yet from the current feed");
                ViewportTruthText.TextColor = Color.FromArgb("#FF6B6B");
            }

            var sourceFlavor = parsed.SyntheticSource switch
            {
                true => "synthetic",
                false => "real",
                _ => "unknown"
            };

            SetLabelText(ViewportSourceText, $"Source: {parsed.SourceName} [{parsed.SourceKind}] / {sourceFlavor} copyToken={parsed.CopyToken}");
            SetLabelText(ViewportEvidenceText, $"Evidence: readable={FormatBool(parsed.Readable)} nonBlack={FormatBool(parsed.NonBlack)} variance={FormatBool(parsed.Variance)} avgLuma={parsed.AvgLuma} maxLuma={parsed.MaxLuma} nonBlackPct={parsed.NonBlackPct} checksum={parsed.Checksum} lastGoodFrame={parsed.LastGoodFrame} event={DefaultText(parsed.Event, "n/a")}");
            SetLabelText(ViewportGeometryText, $"Viewport: size={parsed.Size} sourceSize={parsed.SourceSize} srcPitch={parsed.SourcePitch} presents={parsed.PresentCount} nativeLayer={FormatBool(parsed.NativeLayer)} drawable={parsed.DrawableToken}");
            SetLabelText(ViewportSamplesText, $"Samples: tl={parsed.TopLeftSample} c={parsed.CenterSample} br={parsed.BottomRightSample}");
            SetLabelText(ViewportProbeText, $"Probe: {DefaultText(parsed.ProbeSummary, "n/a")}");
#if MACCATALYST
            if (state is RemoteViewportSessionState.Active or RemoteViewportSessionState.Ready or RemoteViewportSessionState.Recovering or RemoteViewportSessionState.Lost)
            {
                SetLabelText(ViewportDiagnosticsText, diagnostics);
            }
#else
            SetLabelText(ViewportDiagnosticsText, diagnostics);
#endif
        }

        static string? MatchValue(string diagnostics, string key)
        {
            var quoted = Regex.Match(diagnostics, $@"\b{Regex.Escape(key)}='([^']*)'");
            if (quoted.Success)
            {
                return quoted.Groups[1].Value;
            }

            var plain = Regex.Match(diagnostics, $@"\b{Regex.Escape(key)}=([^\s\]]+)");
            return plain.Success ? plain.Groups[1].Value : null;
        }

        static bool? MatchBool(string diagnostics, string key)
        {
            var value = MatchValue(diagnostics, key);
            return value switch
            {
                "1" => true,
                "0" => false,
                _ => null
            };
        }

        static string MatchProbeSummary(string diagnostics)
        {
            var probe = Regex.Match(diagnostics, @"probe\{(.*)\}");
            return probe.Success ? probe.Groups[1].Value.Trim() : string.Empty;
        }

        static void SetLabelText(Label label, string text)
        {
            if (!string.Equals(label.Text, text, StringComparison.Ordinal))
            {
                label.Text = text;
            }
        }

        static string FormatBool(bool? value) => value.HasValue ? (value.Value ? "yes" : "no") : "?";

        static string DefaultText(string? value, string fallback) => string.IsNullOrWhiteSpace(value) ? fallback : value;
    }
}
