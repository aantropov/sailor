using Microsoft.UI.Windowing;
using Window = Microsoft.Maui.Controls.Window;

namespace SailorEditor.Helpers
{
    static class Win32Helper
    {
        public static OverlappedPresenter GetWindowPresenter(this Window mauiWindow)
        {
            var window = mauiWindow.Handler.PlatformView;
            var MainWindowHandle = WinRT.Interop.WindowNative.GetWindowHandle(window);

            var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(MainWindowHandle);
            var appWindow = AppWindow.GetFromWindowId(windowId);
            var presenter = appWindow.Presenter as OverlappedPresenter;

            return presenter;
        }

        public static Point GetAbsolutePositionWin(this VisualElement visualElement)
        {
#if WINDOWS
            //Microsoft.Maui.Platform.MauiButton
            //Control: FrameworkElement, IControlOverrides, ICustomQueryInterface, IWinRTObject, IDynamicInterfaceCastable, IEquatable<Control>

            Microsoft.UI.Xaml.Window window = (Microsoft.UI.Xaml.Window)App.Current.Windows.First<Window>().Handler.PlatformView;
            var platformview = visualElement.Handler.PlatformView as Microsoft.UI.Xaml.FrameworkElement;
            var mPoint = platformview.TransformToVisual(window.Content).TransformPoint(new Windows.Foundation.Point(0, 0));

            return new Point(mPoint.X, mPoint.Y);
#else
            return new Point();
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