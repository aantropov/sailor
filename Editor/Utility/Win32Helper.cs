using Window = Microsoft.Maui.Controls.Window;
#if WINDOWS
using Microsoft.UI.Windowing;
#endif

namespace SailorEditor.Helpers
{
    static class Win32Helper
    {
#if WINDOWS
        public static OverlappedPresenter GetWindowPresenter(this Window mauiWindow)
        {
            var window = mauiWindow.Handler.PlatformView;
            var MainWindowHandle = WinRT.Interop.WindowNative.GetWindowHandle(window);

            var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(MainWindowHandle);
            var appWindow = AppWindow.GetFromWindowId(windowId);
            var presenter = appWindow.Presenter as OverlappedPresenter;

            return presenter;
        }

#endif
        public static Rect GetAbsolutePositionWin(this VisualElement visualElement)
        {
#if WINDOWS
            //Microsoft.Maui.Platform.MauiButton
            //Control: FrameworkElement, IControlOverrides, ICustomQueryInterface, IWinRTObject, IDynamicInterfaceCastable, IEquatable<Control>

            if (visualElement != null && visualElement.Handler != null)
            {
                Microsoft.UI.Xaml.Window window = (Microsoft.UI.Xaml.Window)App.Current.Windows.First<Window>()?.Handler?.PlatformView;
                if (visualElement.Handler.PlatformView is Microsoft.UI.Xaml.FrameworkElement platformView)
                {
                    var mPoint = platformView.TransformToVisual(window.Content).TransformPoint(new Windows.Foundation.Point(0, 0));

                    var dpi = DeviceDisplay.Current.MainDisplayInfo.Density;

                    double width = platformView.ActualWidth * dpi;
                    double height = platformView.ActualHeight * dpi;

                    return new Rect(mPoint.X * dpi, mPoint.Y * dpi, width, height);
                }
            }

            return new Rect();
#else
            return new Rect();
#endif
        }

        public static IEnumerable<VisualElement> Ancestors(this VisualElement element)
        {
            while (element != null)
            {
                yield return element;
                element = element.Parent as VisualElement;
            }
        }
        public static Point GetAbsolutePosition(this VisualElement visualElement)
        {
            var ancestors = visualElement.Ancestors();
            var x = ancestors.Sum(ancestor => ancestor.X);
            var y = ancestors.Sum(ancestor => ancestor.Y);

            return new Point(x, y);
        }
    };
}