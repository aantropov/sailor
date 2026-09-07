using CoreFoundation;
using UIKit;

namespace SailorEditor.Platforms.MacCatalyst
{
    public static class MacCatalystWindowChrome
    {
        static string currentTitle = "Engine Mode";

        public static void SetTitle(string title)
        {
            currentTitle = string.IsNullOrWhiteSpace(title) ? "Engine Mode" : title;
            DispatchQueue.MainQueue.DispatchAsync(Apply);
            _ = ApplyLater(100);
            _ = ApplyLater(350);
        }

        static async Task ApplyLater(int delayMs)
        {
            await Task.Delay(delayMs);
            Microsoft.Maui.ApplicationModel.MainThread.BeginInvokeOnMainThread(Apply);
        }

        static void Apply()
        {
            foreach (var window in Application.Current?.Windows ?? [])
            {
                window.Title = currentTitle;
            }

            foreach (var scene in UIApplication.SharedApplication.ConnectedScenes)
            {
                if (scene is not UIWindowScene windowScene || windowScene.Titlebar is not { } titlebar)
                    continue;

                windowScene.Title = currentTitle;
                titlebar.TitleVisibility = UITitlebarTitleVisibility.Visible;
                titlebar.Toolbar = null;
            }
        }
    }
}
