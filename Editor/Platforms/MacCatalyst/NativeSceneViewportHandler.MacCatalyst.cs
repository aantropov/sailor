#if MACCATALYST
#nullable enable
using System;
using CoreAnimation;
using CoreGraphics;
using CoreFoundation;
using Foundation;
using GameController;
using Microsoft.Maui.Handlers;
using ObjCRuntime;
using SailorEditor.Controls;
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

        var nextBounds = new CGRect(0, 0, Math.Max(width, 1), Math.Max(height, 1));
        var nextContentsScale = contentsScale > 0 ? (nfloat)contentsScale : UIScreen.MainScreen.Scale;
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
        readonly WeakReference<NativeSceneViewportHandler> owner;
        GCKeyboardInput? keyboardInput;
        GCMouseInput? mouseInput;
        NSObject? keyboardDidConnectToken;
        NSObject? keyboardDidDisconnectToken;
        NSObject? mouseDidConnectToken;
        NSObject? mouseDidDisconnectToken;
        NativeSceneViewportInputModifier activeMouseModifiers = NativeSceneViewportInputModifier.None;
        bool hasPointerSample;
        CGPoint lastPointerSample;

        public NativeSceneViewportPlatformView(NativeSceneViewportHandler handler)
        {
            owner = new WeakReference<NativeSceneViewportHandler>(handler);
            UserInteractionEnabled = true;
            MultipleTouchEnabled = true;

            keyboardDidConnectToken = GCKeyboard.Notifications.ObserveDidConnect((_, _) => AttachKeyboardInput());
            keyboardDidDisconnectToken = GCKeyboard.Notifications.ObserveDidDisconnect((_, _) => AttachKeyboardInput());
            mouseDidConnectToken = GCMouse.Notifications.ObserveDidConnect((_, _) => AttachMouseInput());
            mouseDidDisconnectToken = GCMouse.Notifications.ObserveDidDisconnect((_, _) => AttachMouseInput());
            AttachKeyboardInput();
            AttachMouseInput();
        }

        public override bool CanBecomeFirstResponder => true;

        public void FocusInput()
        {
            BecomeFirstResponder();
            AttachKeyboardInput();
            PublishFocus(true);
        }

        public override void TouchesBegan(NSSet touches, UIEvent? evt)
        {
            if (mouseInput != null)
            {
                base.TouchesBegan(touches, evt);
                return;
            }

            BecomeFirstResponder();
            PublishFocus(true);
            activeMouseModifiers |= NativeSceneViewportInputModifier.MouseLeft;
            PublishTouchButton(touches, 0, true);
            base.TouchesBegan(touches, evt);
        }

        public override void TouchesMoved(NSSet touches, UIEvent? evt)
        {
            if (mouseInput != null)
            {
                base.TouchesMoved(touches, evt);
                return;
            }

            PublishTouchMove(touches);
            base.TouchesMoved(touches, evt);
        }

        public override void TouchesEnded(NSSet touches, UIEvent? evt)
        {
            if (mouseInput != null)
            {
                base.TouchesEnded(touches, evt);
                return;
            }

            PublishTouchButton(touches, 0, false);
            activeMouseModifiers &= ~NativeSceneViewportInputModifier.MouseLeft;
            base.TouchesEnded(touches, evt);
        }

        public override void TouchesCancelled(NSSet touches, UIEvent? evt)
        {
            if (mouseInput != null)
            {
                base.TouchesCancelled(touches, evt);
                return;
            }

            PublishTouchButton(touches, 0, false);
            activeMouseModifiers = NativeSceneViewportInputModifier.None;
            base.TouchesCancelled(touches, evt);
        }

        public override void PressesBegan(NSSet<UIPress> presses, UIPressesEvent evt)
        {
            BecomeFirstResponder();
            AttachKeyboardInput();
            PublishFocus(true);
            PublishPresses(presses, true);
        }

        public override void PressesEnded(NSSet<UIPress> presses, UIPressesEvent evt)
        {
            PublishPresses(presses, false);
        }

        public override void PressesCancelled(NSSet<UIPress> presses, UIPressesEvent evt)
        {
            PublishPresses(presses, false);
        }

        public override void WillMoveToWindow(UIWindow? window)
        {
            base.WillMoveToWindow(window);
            if (window == null)
            {
                ReleaseMouseInput();
                ReleaseKeyboardInput();
                PublishFocus(false);
            }
        }

        void PublishTouchMove(NSSet touches)
        {
            if (TryGetTouchPoint(touches, out var point))
            {
                RecordPointerSample(point);
                PublishPointer(NativeSceneViewportInputKind.PointerMove, point, 0, false);
            }
        }

        void PublishTouchButton(NSSet touches, uint button, bool pressed)
        {
            if (TryGetTouchPoint(touches, out var point))
            {
                RecordPointerSample(point);
                PublishPointer(NativeSceneViewportInputKind.PointerButton, point, button, pressed);
            }
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
            var scale = ContentScaleFactor > 0 ? (double)ContentScaleFactor : UIScreen.MainScreen.Scale;
            var input = new NativeSceneViewportInputEvent(
                kind,
                PointerX: (float)(point.X * scale),
                PointerY: (float)(point.Y * scale),
                Button: button,
                Modifiers: modifiers,
                Pressed: pressed,
                Captured: modifiers != NativeSceneViewportInputModifier.None);
            Publish(input);
        }

        void AttachMouseInput()
        {
            var input = GCMouse.Current?.MouseInput;
            if (ReferenceEquals(input, mouseInput))
            {
                return;
            }

            ReleaseMouseInput();
            mouseInput = input;
            if (mouseInput == null)
            {
                return;
            }

            mouseInput.LeftButton.PressedChangedHandler = (_, _, pressed) => HandleMouseButtonChanged(0, NativeSceneViewportInputModifier.MouseLeft, pressed);
            mouseInput.RightButton.PressedChangedHandler = (_, _, pressed) => HandleMouseButtonChanged(1, NativeSceneViewportInputModifier.MouseRight, pressed);
            if (mouseInput.MiddleButton != null)
            {
                mouseInput.MiddleButton.PressedChangedHandler = (_, _, pressed) => HandleMouseButtonChanged(2, NativeSceneViewportInputModifier.MouseMiddle, pressed);
            }
            mouseInput.MouseMovedHandler = HandleMouseMoved;
        }

        void ReleaseMouseInput()
        {
            if (mouseInput == null)
            {
                return;
            }

            mouseInput.LeftButton.PressedChangedHandler = null;
            mouseInput.RightButton.PressedChangedHandler = null;
            if (mouseInput.MiddleButton != null)
            {
                mouseInput.MiddleButton.PressedChangedHandler = null;
            }
            mouseInput.MouseMovedHandler = null;
            mouseInput = null;
        }

        void HandleMouseMoved(GCMouseInput _, float deltaX, float deltaY)
        {
            var sensitivity = 1.0;
            if (owner.TryGetTarget(out var handler))
            {
                sensitivity = handler.VirtualView?.MouseSensitivity ?? 1.0;
            }

            var point = hasPointerSample
                ? new CGPoint(lastPointerSample.X + (deltaX * sensitivity), lastPointerSample.Y - (deltaY * sensitivity))
                : CGPoint.Empty;

            RecordPointerSample(point);
            PublishPointer(NativeSceneViewportInputKind.PointerMove, point, 0, false);
        }

        void HandleMouseButtonChanged(uint button, NativeSceneViewportInputModifier modifier, bool pressed)
        {
            if (!IsFirstResponder)
            {
                BecomeFirstResponder();
                AttachKeyboardInput();
                PublishFocus(true);
            }

            if (pressed)
            {
                activeMouseModifiers |= modifier;
                var point = hasPointerSample ? lastPointerSample : CGPoint.Empty;
                PublishPointer(NativeSceneViewportInputKind.PointerButton, point, button, true, activeMouseModifiers);
            }
            else
            {
                var eventModifiers = activeMouseModifiers;
                if ((eventModifiers & modifier) == 0)
                {
                    eventModifiers |= modifier;
                }

                var point = hasPointerSample ? lastPointerSample : CGPoint.Empty;
                PublishPointer(NativeSceneViewportInputKind.PointerButton, point, button, false, eventModifiers);
                activeMouseModifiers &= ~modifier;
            }
        }

        void RecordPointerSample(CGPoint point)
        {
            lastPointerSample = point;
            hasPointerSample = true;
        }

        void AttachKeyboardInput()
        {
            var input = GCKeyboard.CoalescedKeyboard?.KeyboardInput;
            if (ReferenceEquals(input, keyboardInput))
            {
                return;
            }

            ReleaseKeyboardInput();
            keyboardInput = input;
            if (keyboardInput != null)
            {
                keyboardInput.KeyChangedHandler = HandleKeyboardKeyChanged;
            }
        }

        void ReleaseKeyboardInput()
        {
            if (keyboardInput != null)
            {
                keyboardInput.KeyChangedHandler = null;
                keyboardInput = null;
            }
        }

        void HandleKeyboardKeyChanged(GCKeyboardInput keyboard, GCControllerButtonInput key, nint keyCode, bool pressed)
        {
            if (!IsFirstResponder)
            {
                return;
            }

            var mappedKey = MapGameControllerKeyCode(keyCode);
            if (mappedKey == 0)
            {
                return;
            }

            var point = hasPointerSample ? lastPointerSample : CGPoint.Empty;
            var scale = ContentScaleFactor > 0 ? (double)ContentScaleFactor : UIScreen.MainScreen.Scale;
            Publish(new NativeSceneViewportInputEvent(
                NativeSceneViewportInputKind.Key,
                PointerX: (float)(point.X * scale),
                PointerY: (float)(point.Y * scale),
                KeyCode: mappedKey,
                Modifiers: activeMouseModifiers,
                Pressed: pressed,
                Focused: true,
                Captured: activeMouseModifiers != NativeSceneViewportInputModifier.None));
        }

        void PublishPresses(NSSet<UIPress> presses, bool pressed)
        {
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

                Publish(new NativeSceneViewportInputEvent(
                    NativeSceneViewportInputKind.Key,
                    KeyCode: keyCode,
                    Modifiers: MapModifiers(press.Key?.ModifierFlags ?? 0),
                    Pressed: pressed));
            }
        }

        void PublishFocus(bool focused)
        {
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

        static uint MapKeyCode(UIKey? key)
        {
            if (key == null)
            {
                return 0;
            }

            try
            {
                var hid = Convert.ToUInt32(key.KeyCode);
                switch (hid)
                {
                    case 0x29:
                        return 0x1B;
                    case 0x3E:
                        return 0x74;
                    case 0x3F:
                        return 0x75;
                    case 0xE0:
                    case 0xE4:
                        return 0x11;
                    case 0xE1:
                    case 0xE5:
                        return 0x10;
                }
            }
            catch (InvalidCastException)
            {
            }

            var text = key?.CharactersIgnoringModifiers;
            if (string.IsNullOrEmpty(text))
            {
                return 0;
            }

            var ch = text[0];
            if (ch >= 'a' && ch <= 'z')
            {
                return (uint)char.ToUpperInvariant(ch);
            }
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
            {
                return ch;
            }

            return ch switch
            {
                ' ' => 0x20,
                '\t' => 0x09,
                '\r' => 0x0D,
                '\u001B' => 0x1B,
                _ => 0
            };
        }

        static uint MapGameControllerKeyCode(nint keyCode)
        {
            if (keyCode >= GCKeyCode.KeyA && keyCode <= GCKeyCode.KeyZ)
            {
                return (uint)('A' + (int)(keyCode - GCKeyCode.KeyA));
            }

            if (keyCode >= GCKeyCode.One && keyCode <= GCKeyCode.Nine)
            {
                return (uint)('1' + (int)(keyCode - GCKeyCode.One));
            }

            return keyCode switch
            {
                var value when value == GCKeyCode.Zero => '0',
                var value when value == GCKeyCode.Spacebar => 0x20,
                var value when value == GCKeyCode.Tab => 0x09,
                var value when value == GCKeyCode.ReturnOrEnter => 0x0D,
                var value when value == GCKeyCode.Escape => 0x1B,
                var value when value == GCKeyCode.LeftShift || value == GCKeyCode.RightShift => 0x10,
                var value when value == GCKeyCode.LeftControl || value == GCKeyCode.RightControl => 0x11,
                var value when value == GCKeyCode.F5 => 0x74,
                var value when value == GCKeyCode.F6 => 0x75,
                _ => 0
            };
        }
    }
}
#endif
