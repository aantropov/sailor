#nullable enable
using Microsoft.Maui.Handlers;
using Microsoft.UI.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using SailorEditor.Controls;
using System.Runtime.InteropServices;
using Windows.System;

namespace SailorEditor.Platforms.Windows;

public sealed class NativeSceneViewportHandler :
    ViewHandler<NativeSceneViewport, SwapChainPanel>,
    INativeSceneViewportLayoutHost
{
    public static readonly IPropertyMapper<NativeSceneViewport, NativeSceneViewportHandler> Mapper =
        new PropertyMapper<NativeSceneViewport, NativeSceneViewportHandler>(ViewMapper)
        {
            [nameof(Microsoft.Maui.IView.Background)] = static (_, _) => { }
        };

    nint hostHandle;
    NativeSceneViewportInputModifier activePointerButtons;
    readonly HashSet<VirtualKey> forwardedKeys = [];
    readonly KeyEventHandler rootKeyDownHandler;
    readonly KeyEventHandler rootKeyUpHandler;
    readonly PointerEventHandler rootPointerPressedHandler;
    UIElement? keyboardRoot;
    bool viewportFocused;
    double lastPublishedWidth = -1;
    double lastPublishedHeight = -1;
    double lastPublishedScale = -1;
    uint lastPublishedDrawableWidth;
    uint lastPublishedDrawableHeight;

    public NativeSceneViewportHandler() : base(Mapper)
    {
        rootKeyDownHandler = OnRootKeyDown;
        rootKeyUpHandler = OnRootKeyUp;
        rootPointerPressedHandler = OnRootPointerPressed;
    }

    protected override SwapChainPanel CreatePlatformView() => new()
    {
        AllowFocusOnInteraction = true
    };

    protected override void ConnectHandler(SwapChainPanel platformView)
    {
        base.ConnectHandler(platformView);

        platformView.Loaded += OnLoaded;
        platformView.Unloaded += OnUnloaded;
        platformView.SizeChanged += OnSizeChanged;
        platformView.CompositionScaleChanged += OnCompositionScaleChanged;
        platformView.PointerEntered += OnPointerEntered;
        platformView.PointerMoved += OnPointerMoved;
        platformView.PointerPressed += OnPointerPressed;
        platformView.PointerReleased += OnPointerReleased;
        platformView.PointerWheelChanged += OnPointerWheelChanged;
        platformView.PointerCaptureLost += OnPointerCaptureLost;
        platformView.GotFocus += OnGotFocus;
        platformView.LostFocus += OnLostFocus;

        if (platformView.IsLoaded)
        {
            AttachKeyboardRoot(platformView);
            PublishHostHandle(platformView);
            PublishLayout(platformView);
        }
    }

    protected override void DisconnectHandler(SwapChainPanel platformView)
    {
        platformView.Loaded -= OnLoaded;
        platformView.Unloaded -= OnUnloaded;
        platformView.SizeChanged -= OnSizeChanged;
        platformView.CompositionScaleChanged -= OnCompositionScaleChanged;
        platformView.PointerEntered -= OnPointerEntered;
        platformView.PointerMoved -= OnPointerMoved;
        platformView.PointerPressed -= OnPointerPressed;
        platformView.PointerReleased -= OnPointerReleased;
        platformView.PointerWheelChanged -= OnPointerWheelChanged;
        platformView.PointerCaptureLost -= OnPointerCaptureLost;
        platformView.GotFocus -= OnGotFocus;
        platformView.LostFocus -= OnLostFocus;

        ReleaseViewportFocus();
        DetachKeyboardRoot();
        ReleaseHostHandle();
        base.DisconnectHandler(platformView);
    }

    public void RequestLayoutUpdate(double width, double height, double contentsScale)
    {
        if (PlatformView is not { } platformView)
        {
            return;
        }

        PublishLayout(
            platformView,
            platformView.ActualWidth > 1 ? platformView.ActualWidth : width,
            platformView.ActualHeight > 1 ? platformView.ActualHeight : height,
            ResolveScale(platformView, contentsScale));
    }

    public void RequestInputFocus()
    {
        if (PlatformView is not { } panel)
        {
            return;
        }

        AttachKeyboardRoot(panel);
        SetViewportFocused(true);
        panel.Focus(FocusState.Programmatic);
    }

    void OnLoaded(object sender, RoutedEventArgs args)
    {
        if (sender is not SwapChainPanel panel)
        {
            return;
        }

        AttachKeyboardRoot(panel);
        PublishHostHandle(panel);
        PublishLayout(panel);
    }

    void OnUnloaded(object sender, RoutedEventArgs args)
    {
        ReleaseViewportFocus();
        DetachKeyboardRoot();
        ReleaseHostHandle();
    }

    void AttachKeyboardRoot(SwapChainPanel panel)
    {
        var root = panel.XamlRoot?.Content as UIElement;
        if (ReferenceEquals(keyboardRoot, root))
        {
            return;
        }

        DetachKeyboardRoot();
        keyboardRoot = root;
        keyboardRoot?.AddHandler(
            UIElement.KeyDownEvent,
            rootKeyDownHandler,
            handledEventsToo: true);
        keyboardRoot?.AddHandler(
            UIElement.KeyUpEvent,
            rootKeyUpHandler,
            handledEventsToo: true);
        keyboardRoot?.AddHandler(
            UIElement.PointerPressedEvent,
            rootPointerPressedHandler,
            handledEventsToo: true);
    }

    void DetachKeyboardRoot()
    {
        keyboardRoot?.RemoveHandler(
            UIElement.KeyDownEvent,
            rootKeyDownHandler);
        keyboardRoot?.RemoveHandler(
            UIElement.KeyUpEvent,
            rootKeyUpHandler);
        keyboardRoot?.RemoveHandler(
            UIElement.PointerPressedEvent,
            rootPointerPressedHandler);
        keyboardRoot = null;
    }

    void OnSizeChanged(object sender, SizeChangedEventArgs args)
    {
        if (sender is SwapChainPanel panel)
        {
            PublishLayout(panel, args.NewSize.Width, args.NewSize.Height, ResolveScale(panel));
        }
    }

    void OnCompositionScaleChanged(SwapChainPanel sender, object args)
    {
        PublishLayout(sender);
    }

    void PublishHostHandle(SwapChainPanel panel)
    {
        if (hostHandle != nint.Zero)
        {
            return;
        }

        hostHandle = Marshal.GetIUnknownForObject(panel);
        VirtualView?.UpdateHostHandle(hostHandle);
    }

    void ReleaseHostHandle()
    {
        if (hostHandle == nint.Zero)
        {
            return;
        }

        VirtualView?.UpdateHostHandle(nint.Zero);
        Marshal.Release(hostHandle);
        hostHandle = nint.Zero;
    }

    void PublishLayout(SwapChainPanel panel)
    {
        PublishLayout(panel, panel.ActualWidth, panel.ActualHeight, ResolveScale(panel));
    }

    void PublishLayout(SwapChainPanel panel, double width, double height, double scale)
    {
        width = Math.Max(0, width);
        height = Math.Max(0, height);
        scale = scale > 0 ? scale : 1;
        var drawableWidth = (uint)Math.Max(1, Math.Round(width * scale));
        var drawableHeight = (uint)Math.Max(1, Math.Round(height * scale));
        if (Math.Abs(lastPublishedWidth - width) < 0.5 &&
            Math.Abs(lastPublishedHeight - height) < 0.5 &&
            Math.Abs(lastPublishedScale - scale) < 0.01 &&
            lastPublishedDrawableWidth == drawableWidth &&
            lastPublishedDrawableHeight == drawableHeight)
        {
            return;
        }

        lastPublishedWidth = width;
        lastPublishedHeight = height;
        lastPublishedScale = scale;
        lastPublishedDrawableWidth = drawableWidth;
        lastPublishedDrawableHeight = drawableHeight;
        VirtualView?.UpdateHostLayout(
            width,
            height,
            scale,
            drawableWidth,
            drawableHeight);
    }

    static double ResolveScale(SwapChainPanel panel, double fallback = 1)
    {
        if (panel.CompositionScaleX > 0 &&
            panel.CompositionScaleY > 0 &&
            Math.Abs(panel.CompositionScaleX - panel.CompositionScaleY) < 0.01)
        {
            return panel.CompositionScaleX;
        }

        return panel.XamlRoot?.RasterizationScale is > 0 and var scale
            ? scale
            : Math.Max(1, fallback);
    }

    void OnPointerEntered(object sender, PointerRoutedEventArgs args)
    {
        if (sender is SwapChainPanel panel)
        {
            PublishPointer(panel, args, NativeSceneViewportInputKind.PointerMove);
        }
    }

    void OnPointerMoved(object sender, PointerRoutedEventArgs args)
    {
        if (sender is SwapChainPanel panel)
        {
            PublishPointer(panel, args, NativeSceneViewportInputKind.PointerMove);
        }
    }

    void OnPointerPressed(object sender, PointerRoutedEventArgs args)
    {
        if (sender is not SwapChainPanel panel)
        {
            return;
        }

        AttachKeyboardRoot(panel);
        var point = args.GetCurrentPoint(panel);
        var (button, modifier) = ResolveChangedButton(point.Properties.PointerUpdateKind);
        activePointerButtons |= modifier;
        SetViewportFocused(true);
        panel.Focus(FocusState.Pointer);
        panel.CapturePointer(args.Pointer);
        PublishPointer(
            panel,
            args,
            NativeSceneViewportInputKind.PointerButton,
            button,
            pressed: true);
        args.Handled = true;
    }

    void OnPointerReleased(object sender, PointerRoutedEventArgs args)
    {
        if (sender is not SwapChainPanel panel)
        {
            return;
        }

        var point = args.GetCurrentPoint(panel);
        var (button, modifier) = ResolveChangedButton(point.Properties.PointerUpdateKind);
        activePointerButtons &= ~modifier;
        PublishPointer(
            panel,
            args,
            NativeSceneViewportInputKind.PointerButton,
            button,
            pressed: false);
        if (modifier == NativeSceneViewportInputModifier.MouseRight)
        {
            ReleaseForwardedKeys();
        }
        if (activePointerButtons == NativeSceneViewportInputModifier.None)
        {
            panel.ReleasePointerCapture(args.Pointer);
        }
        args.Handled = true;
    }

    void OnPointerWheelChanged(object sender, PointerRoutedEventArgs args)
    {
        if (sender is not SwapChainPanel panel)
        {
            return;
        }

        var point = args.GetCurrentPoint(panel);
        var position = ResolveDrawablePosition(panel, point.Position.X, point.Position.Y);
        VirtualView?.PublishInput(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.PointerWheel,
            position.X,
            position.Y,
            WheelDeltaY: point.Properties.MouseWheelDelta,
            Modifiers: ResolveModifiers(args.KeyModifiers) | activePointerButtons));
        args.Handled = true;
    }

    void OnPointerCaptureLost(object sender, PointerRoutedEventArgs args)
    {
        var hadActivePointerButtons =
            activePointerButtons != NativeSceneViewportInputModifier.None;
        activePointerButtons = NativeSceneViewportInputModifier.None;
        ReleaseForwardedKeys();
        if (!hadActivePointerButtons)
        {
            return;
        }

        VirtualView?.PublishInput(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.Capture,
            Captured: false));
    }

    void PublishPointer(
        SwapChainPanel panel,
        PointerRoutedEventArgs args,
        NativeSceneViewportInputKind kind,
        uint button = 0,
        bool pressed = false)
    {
        var point = args.GetCurrentPoint(panel);
        var position = ResolveDrawablePosition(panel, point.Position.X, point.Position.Y);
        VirtualView?.PublishInput(new NativeSceneViewportInputEvent(
            kind,
            position.X,
            position.Y,
            Button: button,
            Modifiers: ResolveModifiers(args.KeyModifiers) | activePointerButtons,
            Pressed: pressed,
            Captured: activePointerButtons != NativeSceneViewportInputModifier.None));
    }

    void OnGotFocus(object sender, RoutedEventArgs args)
    {
        SetViewportFocused(true);
    }

    void OnLostFocus(object sender, RoutedEventArgs args)
    {
        if (activePointerButtons != NativeSceneViewportInputModifier.None)
        {
            return;
        }

        ReleaseViewportFocus();
    }

    void OnRootPointerPressed(object sender, PointerRoutedEventArgs args)
    {
        if (PlatformView is { } panel &&
            args.OriginalSource is DependencyObject source &&
            IsDescendantOf(source, panel))
        {
            return;
        }

        ReleaseViewportFocus();
    }

    void SetViewportFocused(bool focused)
    {
        if (viewportFocused == focused)
        {
            return;
        }

        viewportFocused = focused;
        PublishFocus(focused);
    }

    void ReleaseViewportFocus()
    {
        var hadInputOwnership = viewportFocused ||
            activePointerButtons != NativeSceneViewportInputModifier.None ||
            forwardedKeys.Count > 0;
        viewportFocused = false;
        activePointerButtons = NativeSceneViewportInputModifier.None;
        ReleaseForwardedKeys();
        if (hadInputOwnership)
        {
            PublishFocus(false);
        }
    }

    void PublishFocus(bool focused)
    {
        VirtualView?.PublishInput(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.Focus,
            Focused: focused));
    }

    void OnRootKeyDown(object sender, KeyRoutedEventArgs args)
    {
        if (!ShouldForwardKey(args.Key) ||
            (!viewportFocused && !HasRightMouseCapture()))
        {
            return;
        }

        if (forwardedKeys.Add(args.Key))
        {
            PublishKey(args.Key, pressed: true);
        }
        args.Handled = true;
    }

    void OnRootKeyUp(object sender, KeyRoutedEventArgs args)
    {
        if (!ShouldForwardKey(args.Key) ||
            (!forwardedKeys.Remove(args.Key) &&
             !viewportFocused &&
             !HasRightMouseCapture()))
        {
            return;
        }

        PublishKey(args.Key, pressed: false);
        args.Handled = true;
    }

    void ReleaseForwardedKeys()
    {
        foreach (var key in forwardedKeys)
        {
            PublishKey(key, pressed: false);
        }
        forwardedKeys.Clear();
    }

    bool HasRightMouseCapture() =>
        (activePointerButtons & NativeSceneViewportInputModifier.MouseRight) != 0;

    static (float X, float Y) ResolveDrawablePosition(
        SwapChainPanel panel,
        double x,
        double y)
    {
        var scale = ResolveScale(panel);
        return ((float)(x * scale), (float)(y * scale));
    }

    static bool IsDescendantOf(
        DependencyObject source,
        DependencyObject ancestor)
    {
        for (DependencyObject? current = source;
             current is not null;
             current = VisualTreeHelper.GetParent(current))
        {
            if (ReferenceEquals(current, ancestor))
            {
                return true;
            }
        }

        return false;
    }

    static bool ShouldForwardKey(VirtualKey key) =>
        key != VirtualKey.None && (uint)key < 256;

    void PublishKey(VirtualKey key, bool pressed)
    {
        VirtualView?.PublishInput(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.Key,
            KeyCode: (uint)key,
            Modifiers: activePointerButtons,
            Pressed: pressed));
    }

    static (uint Button, NativeSceneViewportInputModifier Modifier) ResolveChangedButton(
        PointerUpdateKind updateKind) => updateKind switch
    {
        PointerUpdateKind.LeftButtonPressed or PointerUpdateKind.LeftButtonReleased =>
            (0, NativeSceneViewportInputModifier.MouseLeft),
        PointerUpdateKind.RightButtonPressed or PointerUpdateKind.RightButtonReleased =>
            (1, NativeSceneViewportInputModifier.MouseRight),
        PointerUpdateKind.MiddleButtonPressed or PointerUpdateKind.MiddleButtonReleased =>
            (2, NativeSceneViewportInputModifier.MouseMiddle),
        _ => (0, NativeSceneViewportInputModifier.None)
    };

    static NativeSceneViewportInputModifier ResolveModifiers(VirtualKeyModifiers modifiers)
    {
        var result = NativeSceneViewportInputModifier.None;
        if ((modifiers & VirtualKeyModifiers.Shift) != 0)
        {
            result |= NativeSceneViewportInputModifier.Shift;
        }
        if ((modifiers & VirtualKeyModifiers.Control) != 0)
        {
            result |= NativeSceneViewportInputModifier.Control;
        }
        if ((modifiers & VirtualKeyModifiers.Menu) != 0)
        {
            result |= NativeSceneViewportInputModifier.Alt;
        }
        if ((modifiers & VirtualKeyModifiers.Windows) != 0)
        {
            result |= NativeSceneViewportInputModifier.Meta;
        }
        return result;
    }
}
