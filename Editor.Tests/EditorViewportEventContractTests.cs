using SailorEditor.Scene;
using SailorEditor.Protocol.Generated;

namespace Editor.Tests;

public sealed class EditorViewportEventContractTests
{
    [Fact]
    public void SelectionEvent_ParsesSelectedAndClearedSelection()
    {
        Assert.True(EditorViewportEventContract.TryParse(
            "kind: selection\nrevision: 17\nmanagedMutationRevision: 5\nselectedInstanceId: go-42\n",
            out var selected,
            out var selectedError), selectedError);
        var selection = Assert.IsType<EditorViewportSelectionEvent>(selected);
        Assert.Equal(17UL, selection.Revision);
        Assert.Equal(5UL, selection.ManagedMutationRevision);
        Assert.Equal("go-42", selection.SelectedInstanceId);

        Assert.True(EditorViewportEventContract.TryParse(
            "kind: selection\nrevision: 18\nmanagedMutationRevision: 5\nselectedInstanceId: ''\n",
            out var cleared,
            out var clearedError), clearedError);
        Assert.Equal(string.Empty, Assert.IsType<EditorViewportSelectionEvent>(cleared).SelectedInstanceId);
    }

    [Fact]
    public void TransformEvent_ParsesTypedOperationSpaceAndNumericTransform()
    {
        const string yaml = """
            kind: transform
            revision: 21
            managedMutationRevision: 8
            instanceId: go-42
            operation: Rotate
            space: Local
            beforePosition: [0, 0, 0, 1]
            beforeRotation: [0, 0, 0, 1]
            beforeScale: [1, 1, 1, 0]
            afterPosition: [1, 2.5, -3, 1]
            afterRotation: [0, 0.70710677, 0, 0.70710677]
            afterScale: [1, 2, 3, 0]
            """;

        Assert.True(EditorViewportEventContract.TryParse(yaml, out var parsed, out var error), error);
        var transform = Assert.IsType<EditorViewportTransformEvent>(parsed);
        Assert.Equal(21UL, transform.Revision);
        Assert.Equal(8UL, transform.ManagedMutationRevision);
        Assert.Equal("go-42", transform.InstanceId);
        Assert.Equal(EditorViewportTransformOperation.Rotate, transform.Operation);
        Assert.Equal(EditorViewportTransformSpace.Local, transform.Space);
        Assert.Equal(new EditorViewportVector4(0, 0, 0, 1), transform.BeforePosition);
        Assert.Equal(new EditorViewportVector4(1, 2.5f, -3, 1), transform.AfterPosition);
        Assert.Equal(new EditorViewportVector4(1, 2, 3, 0), transform.AfterScale);
    }

    [Fact]
    public void TypedTransformEvent_MapsGeneratedProtocolPayload()
    {
        var source = new ViewportEvent
        {
            Revision = 21,
            ManagedMutationRevision = 8,
            Transform = new ViewportTransformEvent
            {
                InstanceId = "go-42",
                Operation = ViewportTransformOperation.Rotate,
                Space = ViewportTransformSpace.Local,
                BeforePosition = new Vector4 { W = 1 },
                BeforeRotation = new Vector4 { W = 1 },
                BeforeScale = new Vector4 { X = 1, Y = 1, Z = 1 },
                AfterPosition = new Vector4 { X = 1, Y = 2.5f, Z = -3, W = 1 },
                AfterRotation = new Vector4 { Y = 0.70710677f, W = 0.70710677f },
                AfterScale = new Vector4 { X = 1, Y = 2, Z = 3 }
            }
        };

        Assert.True(
            EditorViewportEventContract.TryCreate(
                source,
                out var viewportEvent,
                out var error),
            error);
        var transform = Assert.IsType<EditorViewportTransformEvent>(viewportEvent);
        Assert.Equal(21UL, transform.Revision);
        Assert.Equal(8UL, transform.ManagedMutationRevision);
        Assert.Equal(EditorViewportTransformOperation.Rotate, transform.Operation);
        Assert.Equal(EditorViewportTransformSpace.Local, transform.Space);
        Assert.Equal(new EditorViewportVector4(1, 2.5f, -3, 1), transform.AfterPosition);
    }

    [Fact]
    public void TypedTransformEvent_RejectsUnspecifiedEnumsAndNonFiniteVectors()
    {
        var source = new ViewportEvent
        {
            Transform = new ViewportTransformEvent
            {
                InstanceId = "go-42",
                Operation = ViewportTransformOperation.Unspecified,
                Space = ViewportTransformSpace.World
            }
        };

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var unsupported,
                out var unsupportedError));
        Assert.Null(unsupported);
        Assert.Contains("unsupported", unsupportedError, StringComparison.OrdinalIgnoreCase);

        source.Transform.Operation = ViewportTransformOperation.Translate;
        source.Transform.BeforePosition = new Vector4 { X = float.PositiveInfinity };
        source.Transform.BeforeRotation = new Vector4();
        source.Transform.BeforeScale = new Vector4();
        source.Transform.AfterPosition = new Vector4();
        source.Transform.AfterRotation = new Vector4();
        source.Transform.AfterScale = new Vector4();

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var nonFinite,
                out var nonFiniteError));
        Assert.Null(nonFinite);
        Assert.Contains("non-finite", nonFiniteError, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AssetDropEvent_MapsTypedAndYamlPayloads()
    {
        var source = new ViewportEvent
        {
            Revision = 31,
            ManagedMutationRevision = 9,
            AssetDrop = new ViewportAssetDropEvent
            {
                FileId = "{12345678-1234-1234-1234-123456789ABC}",
                NormalizedX = 0.25f,
                NormalizedY = 0.75f
            }
        };

        Assert.True(
            EditorViewportEventContract.TryCreate(
                source,
                out var typedEvent,
                out var typedError),
            typedError);
        var typedDrop =
            Assert.IsType<EditorViewportAssetDropEvent>(typedEvent);
        Assert.Equal(31ul, typedDrop.Revision);
        Assert.Equal(9ul, typedDrop.ManagedMutationRevision);
        Assert.Equal(source.AssetDrop.FileId, typedDrop.FileId);
        Assert.Equal(0.25f, typedDrop.NormalizedX);
        Assert.Equal(0.75f, typedDrop.NormalizedY);

        Assert.True(
            EditorViewportEventContract.TryParse(
                """
                kind: assetDrop
                revision: 32
                managedMutationRevision: 10
                fileId: "{12345678-1234-1234-1234-123456789ABC}"
                normalizedX: 0
                normalizedY: 1
                """,
                out var yamlEvent,
                out var yamlError),
            yamlError);
        var yamlDrop =
            Assert.IsType<EditorViewportAssetDropEvent>(yamlEvent);
        Assert.Equal(0f, yamlDrop.NormalizedX);
        Assert.Equal(1f, yamlDrop.NormalizedY);
    }

    [Fact]
    public void AssetDropEvent_RejectsMissingIdentityAndInvalidCoordinates()
    {
        var source = new ViewportEvent
        {
            AssetDrop = new ViewportAssetDropEvent
            {
                FileId = string.Empty,
                NormalizedX = 0.5f,
                NormalizedY = 0.5f
            }
        };

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var missingId,
                out var missingIdError));
        Assert.Null(missingId);
        Assert.Contains(
            "file",
            missingIdError,
            StringComparison.OrdinalIgnoreCase);

        source.AssetDrop.FileId =
            "{12345678-1234-1234-1234-123456789ABC}";
        source.AssetDrop.NormalizedX = float.NaN;
        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var nonFinite,
                out var nonFiniteError));
        Assert.Null(nonFinite);
        Assert.Contains(
            "normalized",
            nonFiniteError,
            StringComparison.OrdinalIgnoreCase);

        Assert.False(
            EditorViewportEventContract.TryParse(
                """
                kind: assetDrop
                revision: 33
                managedMutationRevision: 10
                fileId: "{12345678-1234-1234-1234-123456789ABC}"
                normalizedX: -0.1
                normalizedY: 0.5
                """,
                out var outOfRange,
                out var outOfRangeError));
        Assert.Null(outOfRange);
        Assert.Contains(
            "normalized",
            outOfRangeError,
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ToolShortcutEvent_MapsTypedAndYamlPayloads()
    {
        var source = new ViewportEvent
        {
            Revision = 34,
            ManagedMutationRevision = 12,
            ToolShortcut = new ViewportToolShortcutEvent
            {
                KeyCode = 'W'
            }
        };

        Assert.True(
            EditorViewportEventContract.TryCreate(
                source,
                out var typedEvent,
                out var typedError),
            typedError);
        var typedShortcut =
            Assert.IsType<EditorViewportToolShortcutEvent>(typedEvent);
        Assert.Equal(34ul, typedShortcut.Revision);
        Assert.Equal(12ul, typedShortcut.ManagedMutationRevision);
        Assert.Equal((uint)'W', typedShortcut.KeyCode);

        Assert.True(
            EditorViewportEventContract.TryParse(
                """
                kind: toolShortcut
                revision: 35
                managedMutationRevision: 13
                keyCode: 84
                """,
                out var yamlEvent,
                out var yamlError),
            yamlError);
        var yamlShortcut =
            Assert.IsType<EditorViewportToolShortcutEvent>(yamlEvent);
        Assert.Equal((uint)'T', yamlShortcut.KeyCode);
    }

    [Fact]
    public void ToolShortcutEvent_RejectsUnsupportedKeys()
    {
        var source = new ViewportEvent
        {
            ToolShortcut = new ViewportToolShortcutEvent
            {
                KeyCode = 'X'
            }
        };

        Assert.False(
            EditorViewportEventContract.TryCreate(
                source,
                out var typedEvent,
                out var typedError));
        Assert.Null(typedEvent);
        Assert.Contains(
            "unsupported",
            typedError,
            StringComparison.OrdinalIgnoreCase);

        Assert.False(
            EditorViewportEventContract.TryParse(
                """
                kind: toolShortcut
                revision: 36
                managedMutationRevision: 13
                keyCode: 119
                """,
                out var yamlEvent,
                out var yamlError));
        Assert.Null(yamlEvent);
        Assert.Contains(
            "unsupported",
            yamlError,
            StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("kind: unknown\nrevision: 1\nmanagedMutationRevision: 0\n", "Unsupported viewport event kind")]
    [InlineData("kind: selection\nrevision: 1\nmanagedMutationRevision: 0\nselectedInstanceId: go\nunexpected: true\n", "unexpected field")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: ''\noperation: Translate\nspace: World\nbeforePosition: [0, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "instanceId")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: translate\nspace: World\nbeforePosition: [0, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "unsupported value")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: Scale\nspace: World\nbeforePosition: [0, 0, 0]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "exactly four")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: Scale\nspace: World\nbeforePosition: [.inf, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "invalid number")]
    public void InvalidPayload_IsRejected(string yaml, string expectedError)
    {
        Assert.False(EditorViewportEventContract.TryParse(yaml, out var parsed, out var error));
        Assert.Null(parsed);
        Assert.Contains(expectedError, error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RevisionGate_RejectsDuplicateAndStaleEventsAcrossKinds()
    {
        var gate = new EditorViewportEventRevisionGate();
        var revisionSeven =
            new EditorViewportSelectionEvent(7, 0, "go-1");
        var revisionEight = Transform(8);

        Assert.True(gate.TryAccept(revisionSeven));
        Assert.True(gate.IsCurrent(revisionSeven));
        Assert.False(gate.TryAccept(Transform(7)));
        Assert.False(gate.TryAccept(Transform(6)));
        Assert.True(gate.TryAccept(revisionEight));
        Assert.False(gate.IsCurrent(revisionSeven));
        Assert.True(gate.IsCurrent(revisionEight));
        Assert.Equal(8UL, gate.LastAcceptedRevision);

        gate.Reset();
        Assert.Null(gate.LastAcceptedRevision);
        Assert.False(gate.IsCurrent(revisionEight));
        Assert.True(gate.TryAccept(new EditorViewportSelectionEvent(0, 0, string.Empty)));
    }

    [Fact]
    public void ManagedMutationOrder_RejectsDelayedViewportEventAfterNewerManagedEdit()
    {
        const ulong eventManagedMutationRevision = 12;

        Assert.True(EditorViewportMutationOrder.IsCurrent(eventManagedMutationRevision, 12));
        Assert.False(EditorViewportMutationOrder.IsCurrent(eventManagedMutationRevision, 13));
    }

    [Fact]
    public void SceneEvents_CheckNativeMutationFence_AndInvalidateQueuedSnapshots()
    {
        var sceneSource = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var engineSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var targetSource = ReadRepositoryFile("Editor", "Commands", "EditorViewportTransformTarget.cs");

        var selectionApply = Slice(sceneSource, "async Task ApplyViewportSelectionAsync", "async Task ApplyViewportTransformAsync");
        var transformApply = Slice(sceneSource, "async Task ApplyViewportTransformAsync", "ActionContext CreateViewportActionContext");
        Assert.Equal(2, CountOccurrences(selectionApply, "await engineService.IsEditorViewportSelectionEventCurrentAsync("));
        Assert.Equal(2, CountOccurrences(transformApply, "await engineService.IsEditorViewportTransformEventCurrentAsync("));
        AssertFenceIsRecheckedAfterInspectorFlush(
            selectionApply,
            "await engineService.IsEditorViewportSelectionEventCurrentAsync(");
        AssertFenceIsRecheckedAfterInspectorFlush(
            transformApply,
            "await engineService.IsEditorViewportTransformEventCurrentAsync(");
        Assert.Contains("viewportEvent.InstanceId", transformApply, StringComparison.Ordinal);
        Assert.Contains(
            "async Task<bool> IsEditorViewportEventCurrentAsync(",
            engineSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain("=> Task.Run(() =>", engineSource, StringComparison.Ordinal);
        Assert.Contains(
            "await protocolClient.GetEditorManagedMutationRevisionAsync(",
            engineSource,
            StringComparison.Ordinal);
        Assert.Contains("Selection = 1", engineSource, StringComparison.Ordinal);
        Assert.Contains("ObjectTransform = 2", engineSource, StringComparison.Ordinal);

        var invalidate = targetSource.IndexOf("engineService.InvalidateQueuedWorldSnapshots()", StringComparison.Ordinal);
        var applyLocal = targetSource.IndexOf("worldService.ApplyGameObjectYamlLocal", invalidate, StringComparison.Ordinal);
        Assert.True(invalidate >= 0);
        Assert.True(applyLocal > invalidate);
    }

    [Fact]
    public void PrimaryPointerPress_FlushesInspectorBeforeNativeGestureStarts()
    {
        var source = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var inputHandler = Slice(
            source,
            "async Task<NativeViewportInputDispatchResult> ProcessNativeViewportInputAsync(",
            "void SubscribeToEngineLifecycle()");

        Assert.Contains(
            "nativeViewportHost.InputReceived +=\n                    QueueNativeViewportInput;",
            source,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "InputReceived += async",
            source,
            StringComparison.Ordinal);

        AssertInOrder(
            inputHandler,
            "input.Button == 0",
            "await inspectorPendingEditCoordinator",
            ".CommitPendingChangesAsync(cancellationToken)",
            "cancellationToken.ThrowIfCancellationRequested();",
            "RejectPrimaryPointerGesture",
            "isInputCaptured = input.Captured",
            "viewportAdapter.SendInput(");
    }

    [Fact]
    public void ToolShortcutKeys_AlsoReachRuntimeCameraInput()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Views",
            "SceneView.xaml.cs");
        var inputHandler = Slice(
            source,
            "async Task<NativeViewportInputDispatchResult> ProcessNativeViewportInputAsync(",
            "void SubscribeToEngineLifecycle()");
        var shortcutBlock = Slice(
            inputHandler,
            "if (input.Kind == NativeSceneViewportInputKind.Key &&",
            "if (input.Kind ==\n                    NativeSceneViewportInputKind.PointerButton");

        AssertInOrder(
            inputHandler,
            "SceneViewportToolShortcuts.TryApply(",
            "ApplyViewportToolStateAsync(",
            "viewportAdapter.SendInput(");
        Assert.DoesNotContain(
            "return NativeViewportInputDispatchResult.Forwarded;",
            shortcutBlock,
            StringComparison.Ordinal);
    }

    [Fact]
    public void NativeInputQueue_ResetsAcrossViewAndBackendLifecycle()
    {
        var source = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var unload = Slice(
            source,
            "Unloaded += (sender, args) =>\n            {\n                isRunning = false;",
            "        }\n\n        void QueueNativeViewportInput(");
        var lifecycle = Slice(
            source,
            "void OnEngineLifecycleStateChanged(EngineLifecycleState state)",
            "async void OnEditorViewportEvents(");

        Assert.Contains("ResetNativeViewportInputQueue();", unload, StringComparison.Ordinal);
        AssertInOrder(
            lifecycle,
            "viewportEventRevisionGate.Reset();",
            "ResetNativeViewportInputQueue();",
            "viewportAdapter.ResetForBackendRestart();");
        Assert.Contains(
            "nativeViewportInputQueue.Enqueue(input);",
            source,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Task nativeViewportInputQueue",
            source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void MacViewportLayout_UsesCurrentPlatformBoundsInsteadOfStaleManagedSize()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Platforms",
            "MacCatalyst",
            "NativeSceneViewportHandler.MacCatalyst.cs");
        var requestLayout = Slice(
            source,
            "public void RequestLayoutUpdate(double width, double height, double contentsScale)",
            "void UpdatePlatformLayout(");
        var platformLayout = Slice(
            source,
            "public override void LayoutSubviews()",
            "public void FocusInput()");

        AssertInOrder(
            requestLayout,
            "var platformBounds = PlatformView?.Bounds ?? CGRect.Empty;",
            "ResolveLayoutBounds(platformBounds, width, height)",
            "QueueLayoutUpdate(nextBounds, nextContentsScale);");
        AssertInOrder(
            platformLayout,
            "base.LayoutSubviews();",
            "handler.UpdatePlatformLayout(Bounds, ContentScaleFactor);");
        Assert.Contains(
            "var width = platformBounds.Width > 0 ? platformBounds.Width : Math.Max(fallbackWidth, 1);",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "var height = platformBounds.Height > 0 ? platformBounds.Height : Math.Max(fallbackHeight, 1);",
            source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void MacMouseInput_AcquiresFocusOnlyForOwnedPressAndReleasesLifecycleObservers()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Platforms",
            "MacCatalyst",
            "NativeSceneViewportHandler.MacCatalyst.cs");
        var infoPlist = ReadRepositoryFile(
            "Editor",
            "Platforms",
            "MacCatalyst",
            "Info.plist");
        var project = ReadRepositoryFile("Editor", "SailorEditor.csproj");
        var buttonHandler = Slice(source, "void HandleMouseButtonChanged", "void RecordPointerSample");
        var windowLifecycle = Slice(source, "public override void WillMoveToWindow", "protected override void Dispose");
        var touches = Slice(source, "public override void TouchesBegan", "public override void PressesBegan");
        var secondaryDrag = Slice(
            source,
            "void HandleViewportSecondaryPointerDrag(",
            "void PublishTouchMove(");
        var secondaryDragBegan = Slice(
            secondaryDrag,
            "case UIGestureRecognizerState.Began:",
            "case UIGestureRecognizerState.Changed:");
        var secondaryDragChanged = Slice(
            secondaryDrag,
            "case UIGestureRecognizerState.Changed:",
            "case UIGestureRecognizerState.Ended:");
        var uiKitMotion = Slice(
            source,
            "bool PrepareUIKitPointerMotion(CGPoint point)",
            "void ClearActivePointerState()");
        var mouseAttachment = Slice(
            source,
            "void AttachMouseInput()",
            "void ReleaseMouseInput()");

        AssertInOrder(
            buttonHandler,
            "var hasLocalHit = hasActiveHover;",
            "SceneViewportPointerRouting.ShouldAcceptMouseButton(",
            "QueueFocusReleaseIfPointerRemainsOutside();",
            "if (!IsFirstResponder)",
            "FocusInput();",
            "activeMouseModifiers |= modifier",
            "activeMouseModifiers &= ~modifier");
        AssertInOrder(
            windowLifecycle,
            "AttachInputObservers();",
            "AttachKeyboardInput();",
            "AttachMouseInput();",
            "ReleaseMouseInput();",
            "ReleaseKeyboardInput();",
            "ReleaseInputObservers();");
        Assert.Contains("ReferenceEquals(mouseInputOwner, this)", source, StringComparison.Ordinal);
        Assert.Contains("releaseInput = releasedOwner.DetachMouseInputForReplacement();", source, StringComparison.Ordinal);
        Assert.Contains("activeLocalPointerModifier = ResolvePointerModifier(evt);", touches, StringComparison.Ordinal);
        Assert.DoesNotContain("activeMouseModifiers != NativeSceneViewportInputModifier.None", touches, StringComparison.Ordinal);
        Assert.Contains("TryUsePointerMotionSource(PointerMotionSource.UIKit)", source, StringComparison.Ordinal);
        Assert.Contains("TryUsePointerMotionSource(PointerMotionSource.GameController)", source, StringComparison.Ordinal);
        Assert.Contains("HasPointerMoved(point)", source, StringComparison.Ordinal);
        Assert.Contains("deltaX == 0.0f && deltaY == 0.0f", source, StringComparison.Ordinal);
        Assert.Contains("SecondaryPointerDragGestureRecognizer", source, StringComparison.Ordinal);
        Assert.Contains("public override bool ShouldReceive(UIEvent evt)", source, StringComparison.Ordinal);
        Assert.Contains("public override void TouchesBegan(NSSet touches, UIEvent evt)", source, StringComparison.Ordinal);
        Assert.Contains("public override void TouchesMoved(NSSet touches, UIEvent evt)", source, StringComparison.Ordinal);
        Assert.Contains("public override void TouchesEnded(NSSet touches, UIEvent evt)", source, StringComparison.Ordinal);
        Assert.Contains("public override void TouchesCancelled(NSSet touches, UIEvent evt)", source, StringComparison.Ordinal);
        Assert.Contains("(evt.ButtonMask & UIEventButtonMask.Secondary) != 0", source, StringComparison.Ordinal);
        Assert.Contains("HandleViewportSecondaryPointerDrag", source, StringComparison.Ordinal);
        Assert.Contains("<key>UIApplicationSupportsIndirectInputEvents</key>", infoPlist, StringComparison.Ordinal);
        Assert.Contains("<true/>", infoPlist, StringComparison.Ordinal);
        Assert.Contains("<ApplicationManifest Condition=", project, StringComparison.Ordinal);
        Assert.Contains("Platforms\\MacCatalyst\\Info.plist</ApplicationManifest>", project, StringComparison.Ordinal);
        Assert.Contains("PublishLocalPointerButton(point, modifier, true)", source, StringComparison.Ordinal);
        Assert.Contains("PublishLocalPointerButton(point, modifier, false)", source, StringComparison.Ordinal);
        Assert.Contains("PublishTouchButton(touches, activeLocalPointerModifier, false);", touches, StringComparison.Ordinal);
        Assert.Contains("Trackpads can provide GCMouse motion without", touches, StringComparison.Ordinal);
        Assert.DoesNotContain("ObserveDidBecomeCurrent", source, StringComparison.Ordinal);
        Assert.DoesNotContain("ObserveDidStopBeingCurrent", source, StringComparison.Ordinal);
        Assert.Contains("foreach (var mouse in GCMouse.Mice)", mouseAttachment, StringComparison.Ordinal);
        Assert.Contains("input.MouseMovedHandler = HandleMouseMoved;", source, StringComparison.Ordinal);
        Assert.Contains("mouse.HandlerQueue = DispatchQueue.MainQueue;", mouseAttachment, StringComparison.Ordinal);
        AssertInOrder(
            mouseAttachment,
            "releasedOwner.Publish(captureInput);",
            "SynchronizePressedMouseButtons(attachedInput);");
        Assert.Contains("input.LeftButton.IsPressed", mouseAttachment, StringComparison.Ordinal);
        Assert.Contains("input.RightButton.IsPressed", mouseAttachment, StringComparison.Ordinal);
        Assert.Contains("input.MiddleButton?.IsPressed == true", mouseAttachment, StringComparison.Ordinal);
        Assert.DoesNotContain("new CGEvent((CGEventSource?)null)", source, StringComparison.Ordinal);
        Assert.DoesNotContain("TryRecordSystemPointerSample", source, StringComparison.Ordinal);
        Assert.Contains("(deltaX * sensitivity) / scale", source, StringComparison.Ordinal);
        Assert.Contains("(deltaY * sensitivity) / scale", source, StringComparison.Ordinal);
        Assert.DoesNotContain(
            "pointerMotionSource = PointerMotionSource.UIKit;",
            secondaryDragBegan,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "TryUsePointerMotionSource(PointerMotionSource.UIKit)",
            secondaryDragBegan,
            StringComparison.Ordinal);
        Assert.Contains("PrepareUIKitPointerMotion(point)", secondaryDragChanged, StringComparison.Ordinal);
        AssertInOrder(
            uiKitMotion,
            "pointerMotionSource == PointerMotionSource.GameController",
            "pointerMotionSource = PointerMotionSource.UIKit;",
            "RecordPointerSample(point);",
            "return !switchedFromGameController;");
        Assert.Contains("queuedPointerActivityRevision", buttonHandler, StringComparison.Ordinal);
        Assert.Contains("if (!isAttachedToWindow || isDisposed)", source, StringComparison.Ordinal);
        Assert.Contains("protected override void Dispose(bool disposing)", source, StringComparison.Ordinal);
        Assert.Equal(4, CountOccurrences(source, "Token?.Dispose();"));
    }

    [Fact]
    public void NativeViewport_OwnsDropTargetAndUsesPostEventCaptureState()
    {
        var source = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var inputHandler = Slice(
            source,
            "async Task<NativeViewportInputDispatchResult> ProcessNativeViewportInputAsync(",
            "void SubscribeToEngineLifecycle()");

        Assert.Contains(
            "nativeViewportHost.GestureRecognizers.Add(CreateSceneDropGesture());",
            source,
            StringComparison.Ordinal);
        Assert.Contains("isInputCaptured = input.Captured;", inputHandler, StringComparison.Ordinal);
        Assert.DoesNotContain("captured = isInputCaptured;", inputHandler, StringComparison.Ordinal);
        Assert.DoesNotContain("UpdateViewportIntegration();", inputHandler, StringComparison.Ordinal);
    }

    [Fact]
    public void NativeSelectionProjection_IsLocalOnlySoOneBatchCanApplyAThenB()
    {
        var sceneSource = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var commandsSource = ReadRepositoryFile("Editor", "Commands", "EditorWorldCommands.cs");
        var selectionSource = ReadRepositoryFile("Editor", "Services", "SelectionService.cs");

        var selectionApply = Slice(sceneSource, "async Task ApplyViewportSelectionAsync", "async Task ApplyViewportTransformAsync");
        Assert.Contains("new ApplyRuntimeSelectionCommand(selectedId)", selectionApply, StringComparison.Ordinal);
        Assert.DoesNotContain("new SelectObjectCommand", selectionApply, StringComparison.Ordinal);

        var command = Slice(commandsSource, "public sealed class ApplyRuntimeSelectionCommand", "public sealed class UpdateGameObjectCommand");
        Assert.Contains("ApplyRuntimeSelection(_instanceId)", command, StringComparison.Ordinal);

        var localProjection = Slice(selectionSource, "public void ApplyRuntimeSelection", "public void ResetForDocumentChange");
        var suppress = localProjection.IndexOf("Interlocked.Increment(ref suppressRuntimeSelectionSync)", StringComparison.Ordinal);
        var select = localProjection.IndexOf("SelectInstance(instanceId)", StringComparison.Ordinal);
        var release = localProjection.IndexOf("Interlocked.Decrement(ref suppressRuntimeSelectionSync)", StringComparison.Ordinal);
        Assert.True(suppress >= 0);
        Assert.True(select > suppress);
        Assert.True(release > select);
        Assert.DoesNotContain("workspaceResetInProgress", localProjection, StringComparison.Ordinal);
        Assert.Contains(
            "IsWorkspaceResetInProgress => Volatile.Read(ref workspaceResetInProgress) != 0",
            selectionSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "AssetFile assetFile => assetFile.FileId?.Value",
            selectionSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "snapshot.Kind is SelectionTargetKind.GameObject or",
            selectionSource,
            StringComparison.Ordinal);

        const ulong currentSelectionRevision = 4;
        EditorViewportSelectionEvent[] batch =
        [
            new(17, currentSelectionRevision, "go-a"),
            new(18, currentSelectionRevision, "go-b"),
        ];
        var appliedSelectionIds = batch
            .Where(viewportEvent => EditorViewportMutationOrder.IsCurrent(
                viewportEvent.ManagedMutationRevision,
                currentSelectionRevision))
            .Select(viewportEvent => viewportEvent.SelectedInstanceId)
            .ToArray();
        Assert.Equal(["go-a", "go-b"], appliedSelectionIds);
    }

    [Fact]
    public void EpochGate_RejectsBatchesCapturedBeforeDocumentChange()
    {
        var gate = new EditorViewportEventEpochGate();
        var previousDocument = gate.Current;

        Assert.True(gate.IsCurrent(previousDocument));
        Assert.Equal(previousDocument + 1, gate.Advance());
        Assert.False(gate.IsCurrent(previousDocument));
        Assert.True(gate.IsCurrent(gate.Current));
    }

    [Fact]
    public void EnginePolling_UsesTypedProtocolEventsAndDocumentEpochGuards()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var pull = Slice(
            source,
            "async Task<(IReadOnlyList<EditorViewportEvent> Events, long EventEpoch)>",
            "async Task<(string SerializedWorld, long Sequence)> SerializeWorldAsync(");
        Assert.Contains(
            "await protocolClient.PullEditorViewportEventsAsync(",
            pull,
            StringComparison.Ordinal);
        Assert.Contains("EditorViewportEventContract.TryCreate(", pull, StringComparison.Ordinal);
        Assert.DoesNotContain("Marshal.", pull, StringComparison.Ordinal);
        Assert.Contains("eventEpoch = editorViewportEventEpoch.Current", pull, StringComparison.Ordinal);
        Assert.Contains("editorViewportEventEpoch.IsCurrent(eventEpoch)", pull, StringComparison.Ordinal);
        AssertInOrder(
            pull,
            "await protocolClient.PullEditorViewportEventsAsync(",
            "catch (EngineProtocolException exception)",
            "[EngineService] Failed to poll protocol viewport events:",
            "return (Array.Empty<EditorViewportEvent>(), eventEpoch);");

        var publish = Slice(
            source,
            "void PublishEditorViewportEvents",
            "async Task<bool> InvokeRunningInteropAsync");
        Assert.Contains("IsGenerationActive(generation)", publish, StringComparison.Ordinal);
        Assert.Contains("editorViewportEventEpoch.IsCurrent(eventEpoch)", publish, StringComparison.Ordinal);

        var load = Slice(
            source,
            "public async Task<bool> LoadWorldAsync(",
            "public async Task<bool> CreateWorldAsync(");
        var create = Slice(
            source,
            "public async Task<bool> CreateWorldAsync(",
            "public async Task<string> SerializeCurrentWorldAsync(");
        Assert.Contains("editorViewportEventEpoch.Advance()", load, StringComparison.Ordinal);
        Assert.Contains("editorViewportEventEpoch.Advance()", create, StringComparison.Ordinal);
    }

    [Fact]
    public void SceneTransform_UsesNativeBeforeAfterWithOneStructuralSnapshot()
    {
        var source = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var apply = Slice(source, "async Task ApplyViewportTransformAsync", "ActionContext CreateViewportActionContext");

        Assert.Equal(1, CountOccurrences(apply, "CaptureGameObjectStructure(gameObject)"));
        Assert.Contains("viewportEvent.BeforePosition", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.BeforeRotation", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.BeforeScale", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.AfterPosition", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.AfterRotation", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.AfterScale", apply, StringComparison.Ordinal);
        Assert.Contains("Name = structure.Name", source, StringComparison.Ordinal);
        Assert.Contains("InstanceId = new InstanceId(structure.InstanceId)", source, StringComparison.Ordinal);
        Assert.Contains("ParentIndex = structure.ParentIndex", source, StringComparison.Ordinal);
        Assert.Contains("ComponentIndices = new List<int>(structure.ComponentIndices)", source, StringComparison.Ordinal);
    }

    static EditorViewportTransformEvent Transform(ulong revision) => new(
        revision,
        0,
        "go-1",
        EditorViewportTransformOperation.Translate,
        EditorViewportTransformSpace.World,
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(1, 1, 1, 0),
        new EditorViewportVector4(1, 0, 0, 1),
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(1, 1, 1, 0));

    static void AssertFenceIsRecheckedAfterInspectorFlush(string source, string fenceMarker)
    {
        var firstFence = source.IndexOf(fenceMarker, StringComparison.Ordinal);
        var flush = source.IndexOf(
            ".CommitPendingChangesAsync()",
            firstFence,
            StringComparison.Ordinal);
        var secondFence = source.IndexOf(fenceMarker, firstFence + fenceMarker.Length, StringComparison.Ordinal);

        Assert.True(firstFence >= 0);
        Assert.True(flush > firstFence);
        Assert.True(secondFence > flush);
    }

    static void AssertInOrder(string source, params string[] values)
    {
        var offset = 0;
        foreach (var value in values)
        {
            var index = source.IndexOf(value, offset, StringComparison.Ordinal);
            Assert.True(index >= 0, $"Missing or out-of-order source marker: {value}");
            offset = index + value.Length;
        }
    }

    static string Slice(string source, string startMarker, string endMarker)
    {
        var start = source.IndexOf(startMarker, StringComparison.Ordinal);
        var end = source.IndexOf(endMarker, start + startMarker.Length, StringComparison.Ordinal);
        Assert.True(start >= 0, $"Missing source marker: {startMarker}");
        Assert.True(end > start, $"Missing source marker after {startMarker}: {endMarker}");
        return source[start..end];
    }

    static int CountOccurrences(string source, string value)
    {
        var count = 0;
        var offset = 0;
        while ((offset = source.IndexOf(value, offset, StringComparison.Ordinal)) >= 0)
        {
            ++count;
            offset += value.Length;
        }

        return count;
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                Directory.Exists(Path.Combine(current.FullName, "Runtime")))
            {
                return File.ReadAllText(Path.Combine([current.FullName, .. relativePath]))
                    .ReplaceLineEndings("\n");
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
