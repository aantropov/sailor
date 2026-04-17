#if MACCATALYST
#nullable enable
using System;
using CoreAnimation;
using CoreGraphics;
using CoreFoundation;
using Foundation;
using Microsoft.Maui.Handlers;
using ObjCRuntime;
using SailorEditor.Controls;
using UIKit;

namespace SailorEditor.Platforms.MacCatalyst;

public sealed class NativeSceneViewportHandler : ViewHandler<NativeSceneViewport, UIView>, INativeSceneViewportLayoutHost
{
    public static readonly IPropertyMapper<NativeSceneViewport, NativeSceneViewportHandler> Mapper =
        new PropertyMapper<NativeSceneViewport, NativeSceneViewportHandler>(ViewMapper);

    static void PublishLayout(NativeSceneViewport virtualView, double width, double height, double contentsScale)
    {
        virtualView.UpdateHostLayout(width, height, contentsScale > 0 ? contentsScale : UIScreen.MainScreen.Scale);
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

        PublishLayout(virtualView, bounds.Width, bounds.Height, contentsScale);
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
        readonly UIHoverGestureRecognizer hoverRecognizer;
        NativeSceneViewportInputModifier activeMouseModifiers = NativeSceneViewportInputModifier.None;

        public NativeSceneViewportPlatformView(NativeSceneViewportHandler handler)
        {
            owner = new WeakReference<NativeSceneViewportHandler>(handler);
            UserInteractionEnabled = true;
            MultipleTouchEnabled = true;
            hoverRecognizer = new UIHoverGestureRecognizer(this, new Selector("handleHover:"));
            AddGestureRecognizer(hoverRecognizer);
        }

        public override bool CanBecomeFirstResponder => true;

        [Export("handleHover:")]
        public void HandleHover(UIHoverGestureRecognizer recognizer)
        {
            var point = recognizer.LocationInView(this);
            PublishPointer(NativeSceneViewportInputKind.PointerMove, point, 0, false);
        }

        public override void TouchesBegan(NSSet touches, UIEvent? evt)
        {
            BecomeFirstResponder();
            PublishFocus(true);
            activeMouseModifiers |= NativeSceneViewportInputModifier.MouseLeft;
            PublishTouchButton(touches, 0, true);
            base.TouchesBegan(touches, evt);
        }

        public override void TouchesMoved(NSSet touches, UIEvent? evt)
        {
            PublishTouchMove(touches);
            base.TouchesMoved(touches, evt);
        }

        public override void TouchesEnded(NSSet touches, UIEvent? evt)
        {
            activeMouseModifiers &= ~NativeSceneViewportInputModifier.MouseLeft;
            PublishTouchButton(touches, 0, false);
            base.TouchesEnded(touches, evt);
        }

        public override void TouchesCancelled(NSSet touches, UIEvent? evt)
        {
            activeMouseModifiers = NativeSceneViewportInputModifier.None;
            PublishTouchButton(touches, 0, false);
            base.TouchesCancelled(touches, evt);
        }

        public override void PressesBegan(NSSet<UIPress> presses, UIPressesEvent evt)
        {
            BecomeFirstResponder();
            PublishFocus(true);
            PublishPresses(presses, true);
            base.PressesBegan(presses, evt);
        }

        public override void PressesEnded(NSSet<UIPress> presses, UIPressesEvent evt)
        {
            PublishPresses(presses, false);
            base.PressesEnded(presses, evt);
        }

        public override void PressesCancelled(NSSet<UIPress> presses, UIPressesEvent evt)
        {
            PublishPresses(presses, false);
            base.PressesCancelled(presses, evt);
        }

        public override void WillMoveToWindow(UIWindow? window)
        {
            base.WillMoveToWindow(window);
            if (window == null)
            {
                PublishFocus(false);
            }
        }

        void PublishTouchMove(NSSet touches)
        {
            if (TryGetTouchPoint(touches, out var point))
            {
                PublishPointer(NativeSceneViewportInputKind.PointerMove, point, 0, false);
            }
        }

        void PublishTouchButton(NSSet touches, uint button, bool pressed)
        {
            if (TryGetTouchPoint(touches, out var point))
            {
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
            var scale = ContentScaleFactor > 0 ? (double)ContentScaleFactor : UIScreen.MainScreen.Scale;
            var input = new NativeSceneViewportInputEvent(
                kind,
                PointerX: (float)(point.X * scale),
                PointerY: (float)(point.Y * scale),
                Button: button,
                Modifiers: activeMouseModifiers,
                Pressed: pressed);
            Publish(input);
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
    }
}
#endif
