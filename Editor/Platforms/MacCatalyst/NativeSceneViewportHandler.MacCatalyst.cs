#if MACCATALYST
#nullable enable
using System;
using System.Collections.Generic;
using System.Threading;
using CoreAnimation;
using CoreGraphics;
using CoreFoundation;
using Foundation;
using GameController;
using Microsoft.Maui.Handlers;
using ObjCRuntime;
using SailorEditor.Controls;
using SailorEditor.Scene;
using UIKit;

namespace SailorEditor.Platforms.MacCatalyst;

public sealed class NativeSceneViewportHandler : ViewHandler<NativeSceneViewport, UIView>, INativeSceneViewportLayoutHost
{
    public static readonly IPropertyMapper<NativeSceneViewport, NativeSceneViewportHandler> Mapper =
        new PropertyMapper<NativeSceneViewport, NativeSceneViewportHandler>(ViewMapper);

    static void PublishLayout(NativeSceneViewport virtualView, double width, double height, double contentsScale, uint drawableWidth, uint drawableHeight)
    {
        virtualView.UpdateHostLayout(width, height, contentsScale > 0 ? contentsScale : UIScreen.MainScreen.Scale, drawableWidth, drawableHeight);
    }

    CAMetalLayer? metalLayer;
    CGRect pendingBounds;
    nfloat pendingContentsScale;
    bool hasPendingLayout;
    bool layoutFlushQueued;
    bool isConnected;
    CGRect currentBounds;
    nfloat currentContentsScale;

    public NativeSceneViewportHandler() : base(Mapper)
    {
    }

    protected override UIView CreatePlatformView() => new NativeSceneViewportPlatformView(this)
    {
        Opaque = true,
        BackgroundColor = UIColor.FromRGB(18, 18, 18),
        ContentScaleFactor = UIScreen.MainScreen.Scale,
        ClipsToBounds = true
    };

    public void RequestLayoutUpdate(double width, double height, double contentsScale)
    {
        if (!isConnected)
        {
            return;
        }

        var platformBounds = PlatformView?.Bounds ?? CGRect.Empty;
        var nextBounds = ResolveLayoutBounds(platformBounds, width, height);
        var nextContentsScale = contentsScale > 0 ? (nfloat)contentsScale : UIScreen.MainScreen.Scale;
        QueueLayoutUpdate(nextBounds, nextContentsScale);
    }

    void UpdatePlatformLayout(CGRect platformBounds, nfloat contentsScale)
    {
        if (!isConnected || platformBounds.Width <= 0 || platformBounds.Height <= 0)
        {
            return;
        }

        QueueLayoutUpdate(
            ResolveLayoutBounds(platformBounds, platformBounds.Width, platformBounds.Height),
            contentsScale > 0 ? contentsScale : UIScreen.MainScreen.Scale);
    }

    void QueueLayoutUpdate(CGRect nextBounds, nfloat nextContentsScale)
    {
        if (currentBounds.Equals(nextBounds) && Math.Abs((double)(currentContentsScale - nextContentsScale)) < 0.01)
        {
            return;
        }

        pendingBounds = nextBounds;
        pendingContentsScale = nextContentsScale;
        hasPendingLayout = true;

        if (layoutFlushQueued)
        {
            return;
        }

        layoutFlushQueued = true;
        DispatchQueue.MainQueue.DispatchAsync(FlushPendingLayout);
    }

    static CGRect ResolveLayoutBounds(CGRect platformBounds, double fallbackWidth, double fallbackHeight)
    {
        var width = platformBounds.Width > 0 ? platformBounds.Width : Math.Max(fallbackWidth, 1);
        var height = platformBounds.Height > 0 ? platformBounds.Height : Math.Max(fallbackHeight, 1);
        return new CGRect(0, 0, width, height);
    }

    public void RequestInputFocus()
    {
        if (PlatformView is NativeSceneViewportPlatformView platformView)
        {
            DispatchQueue.MainQueue.DispatchAsync(platformView.FocusInput);
        }
    }

    void FlushPendingLayout()
    {
        layoutFlushQueued = false;
        if (!isConnected || !hasPendingLayout)
        {
            return;
        }

        hasPendingLayout = false;
        var bounds = pendingBounds;
        var contentsScale = pendingContentsScale > 0 ? pendingContentsScale : UIScreen.MainScreen.Scale;
        UpdateMetalLayerFrame(bounds, contentsScale);
        currentBounds = bounds;
        currentContentsScale = contentsScale;

        var virtualView = VirtualView;
        if (virtualView is null)
        {
            return;
        }

        var drawableSize = metalLayer?.DrawableSize ?? CGSize.Empty;
        var drawableWidth = (uint)Math.Max(1, Math.Round(drawableSize.Width));
        var drawableHeight = (uint)Math.Max(1, Math.Round(drawableSize.Height));
        PublishLayout(virtualView, bounds.Width, bounds.Height, contentsScale, drawableWidth, drawableHeight);
    }

    protected override void ConnectHandler(UIView platformView)
    {
        base.ConnectHandler(platformView);

        isConnected = true;
        pendingBounds = platformView.Bounds;
        pendingContentsScale = platformView.ContentScaleFactor > 0 ? platformView.ContentScaleFactor : UIScreen.MainScreen.Scale;
        hasPendingLayout = true;

        metalLayer = new CAMetalLayer
        {
            Opaque = true,
            Frame = platformView.Bounds,
            ContentsScale = pendingContentsScale
        };
        platformView.Layer.AddSublayer(metalLayer);

        FlushPendingLayout();
        VirtualView?.UpdateHostHandle(metalLayer?.Handle ?? nint.Zero);
    }

    void UpdateMetalLayerFrame(CGRect bounds, nfloat contentsScale)
    {
        if (metalLayer == null)
        {
            return;
        }

        metalLayer.Frame = bounds;
        metalLayer.ContentsScale = contentsScale > 0 ? contentsScale : UIScreen.MainScreen.Scale;
        metalLayer.DrawableSize = new CGSize(
            Math.Max(bounds.Width * metalLayer.ContentsScale, 1),
            Math.Max(bounds.Height * metalLayer.ContentsScale, 1));
    }

    protected override void DisconnectHandler(UIView platformView)
    {
        if (platformView is NativeSceneViewportPlatformView nativePlatformView)
        {
            nativePlatformView.DisconnectInput();
        }

        isConnected = false;
        hasPendingLayout = false;
        layoutFlushQueued = false;
        currentBounds = CGRect.Empty;
        currentContentsScale = 0;
        VirtualView?.UpdateHostHandle(nint.Zero);
        metalLayer?.RemoveFromSuperLayer();
        metalLayer?.Dispose();
        metalLayer = null;
        base.DisconnectHandler(platformView);
    }

    void PublishInput(NativeSceneViewportInputEvent input)
    {
        VirtualView?.PublishInput(input);
    }

    sealed class NativeSceneViewportPlatformView : UIView
    {
        enum PointerMotionSource
        {
            None,
            UIKit,
            GameController
        }

        sealed class ViewportTextInput : UITextField
        {
            readonly NativeSceneViewportPlatformView viewport;
            NSObject? textChangedObserver;

            public ViewportTextInput(NativeSceneViewportPlatformView viewport)
            {
                this.viewport = viewport;
                Hidden = true;
                AutocapitalizationType = UITextAutocapitalizationType.None;
                AutocorrectionType = UITextAutocorrectionType.No;
                SpellCheckingType = UITextSpellCheckingType.No;
                SmartDashesType = UITextSmartDashesType.No;
                SmartQuotesType = UITextSmartQuotesType.No;
                SmartInsertDeleteType = UITextSmartInsertDeleteType.No;
                ShouldReturn = _ => false;
                EditingDidEnd += OnEditingEnded;
            }

            public void ConnectInput()
            {
                textChangedObserver ??= UITextField.Notifications.ObserveTextFieldTextDidChange(
                    this, (_, _) => PublishCommittedText());
            }

            public void DisconnectInput()
            {
                textChangedObserver?.Dispose();
                textChangedObserver = null;
                Text = string.Empty;
                ResignFirstResponder();
            }

            public override void PressesBegan(NSSet<UIPress> presses, UIPressesEvent evt)
            {
                var wasComposing = MarkedTextRange != null;
                base.PressesBegan(presses, evt);
                viewport.PublishPresses(presses, true, wasComposing || MarkedTextRange != null);
            }

            public override void PressesEnded(NSSet<UIPress> presses, UIPressesEvent evt)
            {
                base.PressesEnded(presses, evt);
                viewport.PublishPresses(presses, false);
            }

            public override void PressesCancelled(NSSet<UIPress> presses, UIPressesEvent evt)
            {
                base.PressesCancelled(presses, evt);
                viewport.PublishPresses(presses, false);
            }

            public override bool CanPerform(Selector action, NSObject? sender)
            {
                // The engine handles the paste shortcut; do not insert it twice.
                if (action.Name == "paste:")
                {
                    return false;
                }

                return base.CanPerform(action, sender);
            }

            void PublishCommittedText()
            {
                // UIKit owns composition. The engine receives only the committed
                // text, not intermediate dead-key or IME candidates.
                var committedText = Text;
                if (!viewport.isAttachedToWindow || viewport.isDisposed ||
                    !IsFirstResponder || MarkedTextRange != null || string.IsNullOrEmpty(committedText))
                {
                    return;
                }

                Text = string.Empty;
                viewport.Publish(new NativeSceneViewportInputEvent(
                    NativeSceneViewportInputKind.Text,
                    Text: committedText));
            }

            void OnEditingEnded(object? sender, EventArgs args)
            {
                Text = string.Empty;
                viewport.ReleaseActivePointerState();
                viewport.PublishFocus(false);
            }

            protected override void Dispose(bool disposing)
            {
                if (disposing)
                {
                    DisconnectInput();
                    EditingDidEnd -= OnEditingEnded;
                    ShouldReturn = null;
                }

                base.Dispose(disposing);
            }
        }

        sealed class SecondaryPointerDragGestureRecognizer : UIGestureRecognizer
        {
            readonly Action<UIGestureRecognizerState, CGPoint> publish;
            CGPoint lastPoint;

            public SecondaryPointerDragGestureRecognizer(
                Action<UIGestureRecognizerState, CGPoint> publish)
            {
                this.publish = publish;
            }

            public override bool ShouldReceive(UIEvent evt)
            {
                var accepted =
                    (evt.ButtonMask & UIEventButtonMask.Secondary) != 0;
                return accepted;
            }

            public override void TouchesBegan(NSSet touches, UIEvent evt)
            {
                if (!TryGetPoint(touches, out lastPoint))
                {
                    State = UIGestureRecognizerState.Failed;
                    return;
                }

                State = UIGestureRecognizerState.Began;
                publish(UIGestureRecognizerState.Began, lastPoint);
            }

            public override void TouchesMoved(NSSet touches, UIEvent evt)
            {
                if (TryGetPoint(touches, out var point))
                {
                    lastPoint = point;
                }

                State = UIGestureRecognizerState.Changed;
                publish(UIGestureRecognizerState.Changed, lastPoint);
            }

            public override void TouchesEnded(NSSet touches, UIEvent evt)
            {
                if (TryGetPoint(touches, out var point))
                {
                    lastPoint = point;
                }

                publish(UIGestureRecognizerState.Ended, lastPoint);
                State = UIGestureRecognizerState.Ended;
            }

            public override void TouchesCancelled(NSSet touches, UIEvent evt)
            {
                publish(UIGestureRecognizerState.Cancelled, lastPoint);
                State = UIGestureRecognizerState.Cancelled;
            }

            bool TryGetPoint(NSSet touches, out CGPoint point)
            {
                if (touches.AnyObject is UITouch touch)
                {
                    point = touch.LocationInView(View);
                    return true;
                }

                point = lastPoint;
                return false;
            }
        }

        static readonly object inputOwnershipGate = new();
        static NativeSceneViewportPlatformView? mouseInputOwner;

        readonly WeakReference<NativeSceneViewportHandler> owner;
        readonly ViewportTextInput textInput;
        readonly Dictionary<nint, GCMouseInput> mouseInputs = new();
        NSObject? mouseDidConnectToken;
        NSObject? mouseDidDisconnectToken;
        readonly UIHoverGestureRecognizer hoverGesture;
        readonly SecondaryPointerDragGestureRecognizer secondaryPointerDragGesture;
        GCMouseInput? activeGameControllerInput;
        NativeSceneViewportInputModifier activeMouseModifiers = NativeSceneViewportInputModifier.None;
        NativeSceneViewportInputModifier leftKeyboardModifiers;
        NativeSceneViewportInputModifier rightKeyboardModifiers;
        NativeSceneViewportInputModifier ActiveKeyboardModifiers => leftKeyboardModifiers | rightKeyboardModifiers;
        NativeSceneViewportInputModifier activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
        bool hasPointerSample;
        bool hasActiveHover;
        bool isAttachedToWindow;
        bool isDisposed;
        bool isInputFocused;
        PointerMotionSource pointerMotionSource;
        long pointerActivityRevision;
        CGPoint lastPointerSample;

        bool HasMouseInputs => mouseInputs.Count != 0;
        bool HasInputFocus => textInput.IsFirstResponder;

        public NativeSceneViewportPlatformView(NativeSceneViewportHandler handler)
        {
            owner = new WeakReference<NativeSceneViewportHandler>(handler);
            UserInteractionEnabled = true;
            MultipleTouchEnabled = true;
            textInput = new ViewportTextInput(this);
            AddSubview(textInput);

            hoverGesture = new UIHoverGestureRecognizer(this, new Selector("handleViewportHover:"));
            AddGestureRecognizer(hoverGesture);

            secondaryPointerDragGesture = new SecondaryPointerDragGestureRecognizer(
                HandleViewportSecondaryPointerDrag)
            {
                CancelsTouchesInView = false,
                DelaysTouchesBegan = false,
                DelaysTouchesEnded = false
            };
            AddGestureRecognizer(secondaryPointerDragGesture);
        }

        public override void LayoutSubviews()
        {
            base.LayoutSubviews();
            if (owner.TryGetTarget(out var handler))
            {
                handler.UpdatePlatformLayout(Bounds, ContentScaleFactor);
            }
        }

        public void FocusInput()
        {
            if (!isAttachedToWindow || isDisposed)
            {
                return;
            }

            if (!HasInputFocus && !textInput.BecomeFirstResponder())
            {
                return;
            }

            PublishFocus(true);
        }

        public override void TouchesBegan(NSSet touches, UIEvent? evt)
        {
            activeLocalPointerModifier = ResolvePointerModifier(evt);
            if (activeLocalPointerModifier != NativeSceneViewportInputModifier.None &&
                (activeMouseModifiers & activeLocalPointerModifier) == 0)
            {
                pointerMotionSource = PointerMotionSource.None;
            }
            PublishTouchButton(touches, activeLocalPointerModifier, true);
            base.TouchesBegan(touches, evt);
        }

        public override void TouchesMoved(NSSet touches, UIEvent? evt)
        {
            if (activeLocalPointerModifier == NativeSceneViewportInputModifier.None &&
                HasMouseInputs)
            {
                base.TouchesMoved(touches, evt);
                return;
            }

            PublishTouchMove(touches);
            base.TouchesMoved(touches, evt);
        }

        public override void TouchesEnded(NSSet touches, UIEvent? evt)
        {
            if (HasMouseInputs)
            {
                // The same UIKit sequence that recovered a press must also
                // release it. Trackpads can provide GCMouse motion without
                // sending the corresponding GCMouse button edge.
                PublishTouchButton(touches, activeLocalPointerModifier, false);
                activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
                base.TouchesEnded(touches, evt);
                return;
            }

            PublishTouchButton(touches, activeLocalPointerModifier, false);
            activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
            base.TouchesEnded(touches, evt);
        }

        public override void TouchesCancelled(NSSet touches, UIEvent? evt)
        {
            if (HasMouseInputs)
            {
                activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
                ReleaseActivePointerState();
                base.TouchesCancelled(touches, evt);
                return;
            }

            PublishTouchButton(touches, activeLocalPointerModifier, false);
            activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
            base.TouchesCancelled(touches, evt);
        }

        public override void WillMoveToWindow(UIWindow? window)
        {
            base.WillMoveToWindow(window);
            if (window == null)
            {
                DisconnectInput();
                return;
            }

            isAttachedToWindow = true;
            textInput.ConnectInput();
            AttachInputObservers();
            AttachMouseInput();
        }

        public void DisconnectInput()
        {
            isAttachedToWindow = false;
            hasActiveHover = false;
            textInput.DisconnectInput();
            ReleaseActivePointerState();
            ReleaseMouseInput();
            ReleaseInputObservers();
            PublishFocus(false);
        }

        void AttachInputObservers()
        {
            mouseDidConnectToken ??= GCMouse.Notifications.ObserveDidConnect((_, _) => AttachMouseInput());
            mouseDidDisconnectToken ??= GCMouse.Notifications.ObserveDidDisconnect((_, _) => AttachMouseInput());
        }

        void ReleaseInputObservers()
        {
            mouseDidConnectToken?.Dispose();
            mouseDidConnectToken = null;
            mouseDidDisconnectToken?.Dispose();
            mouseDidDisconnectToken = null;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing && !isDisposed)
            {
                isDisposed = true;
                DisconnectInput();
                textInput.RemoveFromSuperview();
                textInput.Dispose();
                RemoveGestureRecognizer(hoverGesture);
                hoverGesture.Dispose();
                RemoveGestureRecognizer(secondaryPointerDragGesture);
                secondaryPointerDragGesture.Dispose();
            }

            base.Dispose(disposing);
        }

        [Export("handleViewportHover:")]
        void HandleViewportHover(UIHoverGestureRecognizer gesture)
        {
            if (gesture.State is UIGestureRecognizerState.Ended or
                UIGestureRecognizerState.Cancelled or
                UIGestureRecognizerState.Failed)
            {
                hasActiveHover = false;
                if (pointerMotionSource == PointerMotionSource.UIKit)
                {
                    pointerMotionSource = PointerMotionSource.None;
                }
                if (activeMouseModifiers == NativeSceneViewportInputModifier.None)
                {
                    ResetPointerSample();
                }
                return;
            }
            if (gesture.State is not UIGestureRecognizerState.Began and
                not UIGestureRecognizerState.Changed)
            {
                return;
            }

            hasActiveHover = true;
            var point = gesture.LocationInView(this);
            if (SceneViewportPointerRouting.ShouldPublishHoverMove(activeMouseModifiers))
            {
                pointerMotionSource = PointerMotionSource.None;
            }
            else if (!HasPointerMoved(point) ||
                !TryUsePointerMotionSource(PointerMotionSource.UIKit))
            {
                return;
            }

            RecordPointerSample(point);
            PublishPointer(NativeSceneViewportInputKind.PointerMove, point, 0, false);
        }

        void HandleViewportSecondaryPointerDrag(
            UIGestureRecognizerState state,
            CGPoint point)
        {
            const NativeSceneViewportInputModifier modifier =
                NativeSceneViewportInputModifier.MouseRight;
            switch (state)
            {
                case UIGestureRecognizerState.Began:
                    activeLocalPointerModifier = modifier;
                    if ((activeMouseModifiers & modifier) == 0 &&
                        !PublishLocalPointerButton(point, modifier, true))
                    {
                        activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
                        pointerMotionSource = PointerMotionSource.None;
                        return;
                    }

                    RecordPointerSample(point);
                    break;

                case UIGestureRecognizerState.Changed:
                    if ((activeMouseModifiers & modifier) == 0 ||
                        !HasPointerMoved(point) ||
                        !PrepareUIKitPointerMotion(point))
                    {
                        return;
                    }

                    PublishPointer(
                        NativeSceneViewportInputKind.PointerMove,
                        point,
                        0,
                        false);
                    break;

                case UIGestureRecognizerState.Ended:
                case UIGestureRecognizerState.Cancelled:
                case UIGestureRecognizerState.Failed:
                    if (activeLocalPointerModifier == modifier &&
                        (activeMouseModifiers & modifier) != 0)
                    {
                        PublishLocalPointerButton(point, modifier, false);
                    }
                    if (activeLocalPointerModifier == modifier)
                    {
                        activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
                    }
                    if (pointerMotionSource == PointerMotionSource.UIKit)
                    {
                        pointerMotionSource = PointerMotionSource.None;
                    }
                    break;
            }
        }

        void PublishTouchMove(NSSet touches)
        {
            if (TryGetTouchPoint(touches, out var point))
            {
                if (activeLocalPointerModifier != NativeSceneViewportInputModifier.None &&
                    (!HasPointerMoved(point) ||
                        !PrepareUIKitPointerMotion(point)))
                {
                    return;
                }
                RecordPointerSample(point);
                PublishPointer(NativeSceneViewportInputKind.PointerMove, point, 0, false);
            }
        }

        void PublishTouchButton(
            NSSet touches,
            NativeSceneViewportInputModifier modifier,
            bool pressed)
        {
            if (modifier == NativeSceneViewportInputModifier.None ||
                !TryGetTouchPoint(touches, out var point))
            {
                return;
            }

            PublishLocalPointerButton(point, modifier, pressed);
        }

        bool PublishLocalPointerButton(
            CGPoint point,
            NativeSceneViewportInputModifier modifier,
            bool pressed)
        {
            if (modifier == NativeSceneViewportInputModifier.None)
            {
                return false;
            }

            RecordPointerSample(point);
            if (!SceneViewportPointerRouting.ShouldAcceptMouseButton(
                pressed,
                hasLocalHit: true,
                hasPointerSample,
                activeMouseModifiers,
                modifier))
            {
                return false;
            }

            if (pressed)
            {
                FocusInput();
                if (!HasInputFocus)
                {
                    return false;
                }
                activeMouseModifiers |= modifier;
            }
            else
            {
                activeMouseModifiers &= ~modifier;
                if (activeMouseModifiers == NativeSceneViewportInputModifier.None)
                {
                    pointerMotionSource = PointerMotionSource.None;
                    activeGameControllerInput = null;
                }
            }

            PublishPointer(
                NativeSceneViewportInputKind.PointerButton,
                point,
                ResolvePointerButton(modifier),
                pressed,
                activeMouseModifiers);
            if (!pressed &&
                !hasActiveHover &&
                activeMouseModifiers == NativeSceneViewportInputModifier.None)
            {
                ResetPointerSample();
            }
            return true;
        }

        bool TryGetTouchPoint(NSSet touches, out CGPoint point)
        {
            if (touches.AnyObject is UITouch touch)
            {
                point = touch.LocationInView(this);
                return true;
            }

            point = CGPoint.Empty;
            return false;
        }

        void PublishPointer(NativeSceneViewportInputKind kind, CGPoint point, uint button, bool pressed)
        {
            PublishPointer(kind, point, button, pressed, activeMouseModifiers);
        }

        void PublishPointer(NativeSceneViewportInputKind kind, CGPoint point, uint button, bool pressed, NativeSceneViewportInputModifier modifiers)
        {
            RecordPointerSample(point);
            modifiers |= ActiveKeyboardModifiers;
            var scale = ContentScaleFactor > 0 ? (double)ContentScaleFactor : UIScreen.MainScreen.Scale;
            var input = new NativeSceneViewportInputEvent(
                kind,
                PointerX: (float)(point.X * scale),
                PointerY: (float)(point.Y * scale),
                Button: button,
                Modifiers: modifiers,
                Pressed: pressed,
                Captured: HasMouseCapture(modifiers));
            Publish(input);
        }

        void AttachMouseInput()
        {
            if (!isAttachedToWindow || isDisposed)
            {
                return;
            }

            var availableInputs = new Dictionary<nint, GCMouseInput>();
            foreach (var mouse in GCMouse.Mice)
            {
                mouse.HandlerQueue = DispatchQueue.MainQueue;
                if (mouse.MouseInput is { } input)
                {
                    availableInputs[(nint)input.Handle] = input;
                }
            }

            NativeSceneViewportPlatformView? releasedOwner = null;
            NativeSceneViewportInputEvent? releaseInput = null;
            var attachedInputs = new List<GCMouseInput>();
            lock (inputOwnershipGate)
            {
                if (!isAttachedToWindow || isDisposed)
                {
                    return;
                }

                if (availableInputs.Count != 0 &&
                    mouseInputOwner != null &&
                    !ReferenceEquals(mouseInputOwner, this))
                {
                    releasedOwner = mouseInputOwner;
                    releaseInput = releasedOwner.DetachMouseInputForReplacement();
                    mouseInputOwner = null;
                }

                if (availableInputs.Count != 0 && mouseInputOwner == null)
                {
                    mouseInputOwner = this;
                }

                if (ReferenceEquals(mouseInputOwner, this))
                {
                    var removedInputs = new List<nint>();
                    var lostActiveInput = false;
                    foreach (var pair in mouseInputs)
                    {
                        if (availableInputs.ContainsKey(pair.Key))
                        {
                            continue;
                        }

                        lostActiveInput |= ReferenceEquals(
                            activeGameControllerInput,
                            pair.Value);
                        UnbindMouseInput(pair.Value);
                        removedInputs.Add(pair.Key);
                    }

                    foreach (var inputId in removedInputs)
                    {
                        mouseInputs.Remove(inputId);
                    }

                    if (lostActiveInput)
                    {
                        releasedOwner = this;
                        releaseInput = CreatePointerCaptureReleaseInput();
                        ClearActivePointerState();
                    }

                    foreach (var pair in availableInputs)
                    {
                        if (mouseInputs.ContainsKey(pair.Key))
                        {
                            continue;
                        }

                        BindMouseInput(pair.Value);
                        mouseInputs.Add(pair.Key, pair.Value);
                        attachedInputs.Add(pair.Value);
                    }

                    if (mouseInputs.Count == 0)
                    {
                        mouseInputOwner = null;
                    }
                }
            }

            if (releasedOwner != null && releaseInput is { } captureInput)
            {
                releasedOwner.Publish(captureInput);
            }
            foreach (var attachedInput in attachedInputs)
            {
                SynchronizePressedMouseButtons(attachedInput);
            }
        }

        void BindMouseInput(GCMouseInput input)
        {
            input.LeftButton.PressedChangedHandler = (_, _, pressed) =>
                HandleMouseButtonChanged(
                    input,
                    0,
                    NativeSceneViewportInputModifier.MouseLeft,
                    pressed);
            input.RightButton.PressedChangedHandler = (_, _, pressed) =>
                HandleMouseButtonChanged(
                    input,
                    1,
                    NativeSceneViewportInputModifier.MouseRight,
                    pressed);
            if (input.MiddleButton != null)
            {
                input.MiddleButton.PressedChangedHandler = (_, _, pressed) =>
                    HandleMouseButtonChanged(
                        input,
                        2,
                        NativeSceneViewportInputModifier.MouseMiddle,
                        pressed);
            }
            input.MouseMovedHandler = HandleMouseMoved;
            input.Scroll.ValueChangedHandler = HandleMouseScroll;
        }

        static void UnbindMouseInput(GCMouseInput input)
        {
            input.LeftButton.PressedChangedHandler = null;
            input.RightButton.PressedChangedHandler = null;
            if (input.MiddleButton != null)
            {
                input.MiddleButton.PressedChangedHandler = null;
            }
            input.MouseMovedHandler = null;
            input.Scroll.ValueChangedHandler = null;
        }

        void SynchronizePressedMouseButtons(GCMouseInput input)
        {
            lock (inputOwnershipGate)
            {
                if (!isAttachedToWindow ||
                    isDisposed ||
                    !ReferenceEquals(mouseInputOwner, this) ||
                    !mouseInputs.TryGetValue((nint)input.Handle, out var attachedInput) ||
                    !ReferenceEquals(attachedInput, input))
                {
                    return;
                }
            }

            if (input.LeftButton.IsPressed)
            {
                HandleMouseButtonChanged(
                    input,
                    0,
                    NativeSceneViewportInputModifier.MouseLeft,
                    true);
            }
            if (input.RightButton.IsPressed)
            {
                HandleMouseButtonChanged(
                    input,
                    1,
                    NativeSceneViewportInputModifier.MouseRight,
                    true);
            }
            if (input.MiddleButton?.IsPressed == true)
            {
                HandleMouseButtonChanged(
                    input,
                    2,
                    NativeSceneViewportInputModifier.MouseMiddle,
                    true);
            }
        }

        void ReleaseMouseInput()
        {
            NativeSceneViewportInputEvent? releaseInput = null;
            lock (inputOwnershipGate)
            {
                if (ReferenceEquals(mouseInputOwner, this))
                {
                    releaseInput = DetachMouseInputForReplacement();
                    mouseInputOwner = null;
                }
                else
                {
                    mouseInputs.Clear();
                    ClearActivePointerState();
                }
            }

            if (releaseInput is { } captureInput)
            {
                Publish(captureInput);
            }
        }

        NativeSceneViewportInputEvent? DetachMouseInputForReplacement()
        {
            var releaseInput = CreatePointerCaptureReleaseInput();
            foreach (var input in mouseInputs.Values)
            {
                UnbindMouseInput(input);
            }

            mouseInputs.Clear();
            ClearActivePointerState();
            return releaseInput;
        }

        void HandleMouseMoved(GCMouseInput input, float deltaX, float deltaY)
        {
            if (!SceneViewportPointerRouting.ShouldPublishCapturedMove(
                hasPointerSample,
                activeMouseModifiers) ||
                (deltaX == 0.0f && deltaY == 0.0f) ||
                (activeGameControllerInput != null &&
                    !ReferenceEquals(activeGameControllerInput, input)) ||
                !TryUsePointerMotionSource(PointerMotionSource.GameController))
            {
                return;
            }

            activeGameControllerInput ??= input;
            var sensitivity = 1.0;
            if ((activeMouseModifiers & NativeSceneViewportInputModifier.MouseRight) != 0 &&
                owner.TryGetTarget(out var handler))
            {
                sensitivity = handler.VirtualView?.MouseSensitivity ?? 1.0;
            }

            // GCMouse reports raw deltas, while the viewport event carries
            // backing-pixel coordinates. Normalize here so ContentScaleFactor
            // does not amplify camera motion on Retina displays.
            var scale = ContentScaleFactor > 0 ? (double)ContentScaleFactor : UIScreen.MainScreen.Scale;
            var point = new CGPoint(
                lastPointerSample.X + ((deltaX * sensitivity) / scale),
                lastPointerSample.Y - ((deltaY * sensitivity) / scale));

            PublishPointer(NativeSceneViewportInputKind.PointerMove, point, 0, false);
        }

        void HandleMouseButtonChanged(
            GCMouseInput input,
            uint button,
            NativeSceneViewportInputModifier modifier,
            bool pressed)
        {
            if (!pressed &&
                activeGameControllerInput != null &&
                !ReferenceEquals(activeGameControllerInput, input))
            {
                return;
            }

            var hasLocalHit = hasActiveHover;
            if (!SceneViewportPointerRouting.ShouldAcceptMouseButton(
                pressed,
                hasLocalHit,
                hasPointerSample,
                activeMouseModifiers,
                modifier))
            {
                if (pressed &&
                    !hasLocalHit &&
                    activeMouseModifiers == NativeSceneViewportInputModifier.None)
                {
                    QueueFocusReleaseIfPointerRemainsOutside();
                }
                return;
            }

            if (pressed)
            {
                if (!HasInputFocus)
                {
                    FocusInput();
                    if (!HasInputFocus)
                    {
                        return;
                    }
                }

                if (activeMouseModifiers == NativeSceneViewportInputModifier.None)
                {
                    pointerMotionSource = PointerMotionSource.None;
                    activeGameControllerInput = input;
                }
                activeMouseModifiers |= modifier;
                PublishPointer(NativeSceneViewportInputKind.PointerButton, lastPointerSample, button, true, activeMouseModifiers);
            }
            else
            {
                activeMouseModifiers &= ~modifier;
                PublishPointer(NativeSceneViewportInputKind.PointerButton, lastPointerSample, button, false, activeMouseModifiers);
                if (activeMouseModifiers == NativeSceneViewportInputModifier.None)
                {
                    pointerMotionSource = PointerMotionSource.None;
                    activeGameControllerInput = null;
                }
                if (!hasActiveHover && activeMouseModifiers == NativeSceneViewportInputModifier.None)
                {
                    ResetPointerSample();
                }
            }
        }

        void QueueFocusReleaseIfPointerRemainsOutside()
        {
            var queuedPointerActivityRevision = Interlocked.Read(ref pointerActivityRevision);
            DispatchQueue.MainQueue.DispatchAsync(() =>
            {
                if (!isDisposed &&
                    isAttachedToWindow &&
                    !hasActiveHover &&
                    activeMouseModifiers == NativeSceneViewportInputModifier.None &&
                    Interlocked.Read(ref pointerActivityRevision) == queuedPointerActivityRevision &&
                    HasInputFocus)
                {
                    textInput.ResignFirstResponder();
                }
            });
        }

        void HandleMouseScroll(GCControllerDirectionPad _, float deltaX, float deltaY)
        {
            if (!hasPointerSample ||
                (!hasActiveHover && activeMouseModifiers == NativeSceneViewportInputModifier.None))
            {
                return;
            }

            var scale = ContentScaleFactor > 0 ? (double)ContentScaleFactor : UIScreen.MainScreen.Scale;
            Publish(new NativeSceneViewportInputEvent(
                NativeSceneViewportInputKind.PointerWheel,
                PointerX: (float)(lastPointerSample.X * scale),
                PointerY: (float)(lastPointerSample.Y * scale),
                WheelDeltaX: deltaX,
                WheelDeltaY: deltaY,
                Modifiers: activeMouseModifiers | ActiveKeyboardModifiers,
                Focused: isInputFocused,
                Captured: HasMouseCapture(activeMouseModifiers)));
        }

        void RecordPointerSample(CGPoint point)
        {
            lastPointerSample = point;
            hasPointerSample = true;
            Interlocked.Increment(ref pointerActivityRevision);
        }

        void ResetPointerSample()
        {
            hasPointerSample = false;
            lastPointerSample = CGPoint.Empty;
        }

        bool HasPointerMoved(CGPoint point) =>
            !hasPointerSample ||
            Math.Abs((double)(point.X - lastPointerSample.X)) > 0.0001 ||
            Math.Abs((double)(point.Y - lastPointerSample.Y)) > 0.0001;

        bool TryUsePointerMotionSource(PointerMotionSource source)
        {
            if (pointerMotionSource == PointerMotionSource.None)
            {
                pointerMotionSource = source;
            }
            return pointerMotionSource == source;
        }

        bool PrepareUIKitPointerMotion(CGPoint point)
        {
            var switchedFromGameController =
                pointerMotionSource == PointerMotionSource.GameController;
            pointerMotionSource = PointerMotionSource.UIKit;
            activeGameControllerInput = null;
            if (switchedFromGameController)
            {
                // UIKit reports an absolute point while GCMouse reports
                // deltas. Rebase once when UIKit takes over so the two
                // coordinate streams cannot create a false first movement.
                RecordPointerSample(point);
            }
            return !switchedFromGameController;
        }

        void ClearActivePointerState()
        {
            activeMouseModifiers = NativeSceneViewportInputModifier.None;
            activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
            activeGameControllerInput = null;
            pointerMotionSource = PointerMotionSource.None;
            ResetPointerSample();
        }

        void ReleaseActivePointerState()
        {
            var releaseInput = CreatePointerCaptureReleaseInput();
            activeMouseModifiers = NativeSceneViewportInputModifier.None;
            activeLocalPointerModifier = NativeSceneViewportInputModifier.None;
            activeGameControllerInput = null;
            pointerMotionSource = PointerMotionSource.None;
            if (releaseInput is not { } captureInput)
            {
                return;
            }

            if (!hasActiveHover)
            {
                ResetPointerSample();
            }
            Publish(captureInput);
        }

        NativeSceneViewportInputEvent? CreatePointerCaptureReleaseInput()
        {
            if (activeMouseModifiers == NativeSceneViewportInputModifier.None)
            {
                return null;
            }

            var point = hasPointerSample ? lastPointerSample : CGPoint.Empty;
            var scale = ContentScaleFactor > 0 ? (double)ContentScaleFactor : UIScreen.MainScreen.Scale;
            return new NativeSceneViewportInputEvent(
                NativeSceneViewportInputKind.Capture,
                PointerX: (float)(point.X * scale),
                PointerY: (float)(point.Y * scale),
                Modifiers: ActiveKeyboardModifiers,
                Focused: isInputFocused,
                Captured: false);
        }

        void PublishPresses(NSSet<UIPress> presses, bool pressed, bool isComposing = false)
        {
            if (!isAttachedToWindow || isDisposed || !HasInputFocus)
            {
                return;
            }

            foreach (var item in presses)
            {
                if (item is not UIPress press)
                {
                    continue;
                }

                var keyCode = MapKeyCode(press.Key);
                if (keyCode == 0)
                {
                    continue;
                }
                UpdateKeyboardModifier(keyCode, pressed);
                if (isComposing && keyCode is not (0xA0 or 0xA1 or 0xA2 or 0xA3 or 0xA4 or 0xA5 or 0x5B or 0x5C))
                {
                    continue;
                }

                Publish(new NativeSceneViewportInputEvent(
                    NativeSceneViewportInputKind.Key,
                    KeyCode: keyCode,
                    Modifiers: activeMouseModifiers |
                        ActiveKeyboardModifiers |
                        MapModifiers(press.Key?.ModifierFlags ?? 0),
                    Pressed: pressed));
            }
        }

        void PublishFocus(bool focused)
        {
            if (isInputFocused == focused)
            {
                return;
            }

            isInputFocused = focused;
            if (!focused)
            {
                ClearKeyboardModifiers();
            }
            Publish(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.Focus, Focused: focused));
        }

        void Publish(NativeSceneViewportInputEvent input)
        {
            if (owner.TryGetTarget(out var handler))
            {
                handler.PublishInput(input);
            }
        }

        static NativeSceneViewportInputModifier MapModifiers(UIKeyModifierFlags flags)
        {
            var result = NativeSceneViewportInputModifier.None;
            if ((flags & UIKeyModifierFlags.Shift) != 0)
            {
                result |= NativeSceneViewportInputModifier.Shift;
            }
            if ((flags & UIKeyModifierFlags.Control) != 0)
            {
                result |= NativeSceneViewportInputModifier.Control;
            }
            if ((flags & UIKeyModifierFlags.Alternate) != 0)
            {
                result |= NativeSceneViewportInputModifier.Alt;
            }
            if ((flags & UIKeyModifierFlags.Command) != 0)
            {
                result |= NativeSceneViewportInputModifier.Meta;
            }
            return result;
        }

        static bool HasMouseCapture(NativeSceneViewportInputModifier modifiers)
        {
            const NativeSceneViewportInputModifier mouseModifiers =
                NativeSceneViewportInputModifier.MouseLeft |
                NativeSceneViewportInputModifier.MouseRight |
                NativeSceneViewportInputModifier.MouseMiddle;
            return (modifiers & mouseModifiers) != 0;
        }

        static NativeSceneViewportInputModifier ResolvePointerModifier(UIEvent? evt)
        {
            if (evt == null)
            {
                return NativeSceneViewportInputModifier.MouseLeft;
            }

            var buttonMask = (ulong)evt.ButtonMask;
            if ((buttonMask & (1UL << 2)) != 0)
            {
                return NativeSceneViewportInputModifier.MouseMiddle;
            }
            if ((evt.ButtonMask & UIEventButtonMask.Secondary) != 0)
            {
                return NativeSceneViewportInputModifier.MouseRight;
            }
            if ((evt.ButtonMask & UIEventButtonMask.Primary) != 0)
            {
                return NativeSceneViewportInputModifier.MouseLeft;
            }
            return NativeSceneViewportInputModifier.None;
        }

        static uint ResolvePointerButton(NativeSceneViewportInputModifier modifier) => modifier switch
        {
            NativeSceneViewportInputModifier.MouseLeft => 0,
            NativeSceneViewportInputModifier.MouseRight => 1,
            NativeSceneViewportInputModifier.MouseMiddle => 2,
            _ => 0
        };

        void ClearKeyboardModifiers()
        {
            leftKeyboardModifiers = NativeSceneViewportInputModifier.None;
            rightKeyboardModifiers = NativeSceneViewportInputModifier.None;
        }

        void UpdateKeyboardModifier(uint keyCode, bool pressed)
        {
            var modifier = keyCode switch
            {
                0xA0 or 0xA1 => NativeSceneViewportInputModifier.Shift,
                0xA2 or 0xA3 => NativeSceneViewportInputModifier.Control,
                0xA4 or 0xA5 => NativeSceneViewportInputModifier.Alt,
                0x5B or 0x5C => NativeSceneViewportInputModifier.Meta,
                _ => NativeSceneViewportInputModifier.None
            };
            if (modifier == NativeSceneViewportInputModifier.None)
            {
                return;
            }

            if (keyCode is 0xA1 or 0xA3 or 0xA5 or 0x5C)
            {
                rightKeyboardModifiers = pressed ? rightKeyboardModifiers | modifier : rightKeyboardModifiers & ~modifier;
            }
            else
            {
                leftKeyboardModifiers = pressed ? leftKeyboardModifiers | modifier : leftKeyboardModifiers & ~modifier;
            }
        }

        static uint MapKeyCode(UIKey? key)
        {
            if (key == null)
            {
                return 0;
            }

            // Physical keys use HID usages; text and the active keyboard layout
            // are handled separately by UIKit's text responder.
            var hid = (uint)key.KeyCode;
            return hid switch
            {
                >= 0x04 and <= 0x1D => 'A' + hid - 0x04,
                >= 0x1E and <= 0x26 => '1' + hid - 0x1E,
                >= 0x3A and <= 0x45 => 0x70 + hid - 0x3A,
                >= 0x59 and <= 0x61 => 0x61 + hid - 0x59,
                0x27 => '0',
                0x28 or 0x58 => 0x0D,
                0x29 => 0x1B,
                0x2A => 0x08,
                0x2B => 0x09,
                0x2C => 0x20,
                0x2D => 0xBD,
                0x2E => 0xBB,
                0x2F => 0xDB,
                0x30 => 0xDD,
                0x31 => 0xDC,
                0x33 => 0xBA,
                0x34 => 0xDE,
                0x35 => 0xC0,
                0x36 => 0xBC,
                0x37 => 0xBE,
                0x38 => 0xBF,
                0x39 => 0x14,
                0x46 => 0x2C,
                0x47 => 0x91,
                0x48 => 0x13,
                0x49 => 0x2D,
                0x4A => 0x24,
                0x4B => 0x21,
                0x4C => 0x2E,
                0x4D => 0x23,
                0x4E => 0x22,
                0x4F => 0x27,
                0x50 => 0x25,
                0x51 => 0x28,
                0x52 => 0x26,
                0x53 => 0x90,
                0x54 => 0x6F,
                0x55 => 0x6A,
                0x56 => 0x6D,
                0x57 => 0x6B,
                0x62 => 0x60,
                0x63 => 0x6E,
                0x64 => 0xE2,
                0x65 => 0x5D,
                0xE0 => 0xA2,
                0xE1 => 0xA0,
                0xE2 => 0xA4,
                0xE3 => 0x5B,
                0xE4 => 0xA3,
                0xE5 => 0xA1,
                0xE6 => 0xA5,
                0xE7 => 0x5C,
                _ => 0
            };
        }
    }
}
#endif
