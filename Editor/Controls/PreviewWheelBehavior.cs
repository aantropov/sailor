#if MACCATALYST
using CoreGraphics;
using UIKit;
#elif WINDOWS
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Input;
#endif

namespace SailorEditor.Controls;

internal sealed class PreviewWheelBehavior(Action<double> scroll) : Behavior<View>
{
    View? owner;
    bool loaded;
#if MACCATALYST
    UIView? nativeView;
    UIPanGestureRecognizer? wheel;
#elif WINDOWS
    FrameworkElement? nativeView;
#endif

    protected override void OnAttachedTo(View bindable)
    {
        base.OnAttachedTo(bindable);
        owner = bindable;
        bindable.Loaded += OnLoaded;
        bindable.Unloaded += OnUnloaded;
        bindable.HandlerChanged += OnHandlerChanged;
    }

    protected override void OnDetachingFrom(View bindable)
    {
        DetachNative();
        bindable.Loaded -= OnLoaded;
        bindable.Unloaded -= OnUnloaded;
        bindable.HandlerChanged -= OnHandlerChanged;
        owner = null;
        loaded = false;
        base.OnDetachingFrom(bindable);
    }

    void OnLoaded(object? sender, EventArgs args)
    {
        loaded = true;
        AttachNative();
    }

    void OnUnloaded(object? sender, EventArgs args)
    {
        loaded = false;
        DetachNative();
    }

    void OnHandlerChanged(object? sender, EventArgs args) => AttachNative();

    void AttachNative()
    {
        DetachNative();
        if (!loaded)
        {
            return;
        }
#if MACCATALYST
        nativeView = owner?.Handler?.PlatformView as UIView;
        if (nativeView is null)
        {
            return;
        }
        wheel = new UIPanGestureRecognizer(OnScroll)
        {
            AllowedScrollTypesMask = UIScrollTypeMask.All,
            AllowedTouchTypes = [],
            MinimumNumberOfTouches = 0,
            MaximumNumberOfTouches = 0,
            CancelsTouchesInView = false
        };
        nativeView.AddGestureRecognizer(wheel);
#elif WINDOWS
        nativeView = owner?.Handler?.PlatformView as FrameworkElement;
        if (nativeView is not null)
        {
            nativeView.PointerWheelChanged += OnScroll;
        }
#endif
    }

    void DetachNative()
    {
#if MACCATALYST
        if (wheel is not null)
        {
            nativeView?.RemoveGestureRecognizer(wheel);
            wheel.Dispose();
            wheel = null;
        }
        nativeView = null;
#elif WINDOWS
        if (nativeView is not null)
        {
            nativeView.PointerWheelChanged -= OnScroll;
        }
        nativeView = null;
#endif
    }

#if MACCATALYST
    void OnScroll(UIPanGestureRecognizer gesture)
    {
        if (nativeView is null || gesture.State is not (UIGestureRecognizerState.Began or UIGestureRecognizerState.Changed))
        {
            return;
        }
        var translation = gesture.TranslationInView(nativeView);
        gesture.SetTranslation(CGPoint.Empty, nativeView);
        if (translation.Y != 0)
        {
            scroll((double)translation.Y / 40.0);
        }
    }
#elif WINDOWS
    void OnScroll(object sender, PointerRoutedEventArgs args)
    {
        if (nativeView is null)
        {
            return;
        }
        var point = args.GetCurrentPoint(nativeView);
        if (point.Properties.IsHorizontalMouseWheel)
        {
            return;
        }
        args.Handled = true;
        scroll(point.Properties.MouseWheelDelta / 120.0);
    }
#endif
}
