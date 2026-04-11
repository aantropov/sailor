using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Controls;

namespace SailorEditor.Views
{
    public partial class SceneView : ContentView
    {
        bool isRunning = false;
        bool isFocused = false;
        nint nativeHostHandle = nint.Zero;
        string lastViewportStatusText = string.Empty;
#if MACCATALYST
        const bool UseNativeViewportHost = true;
#else
        const bool UseNativeViewportHost = false;
#endif

        public SceneView()
        {
            InitializeComponent();

            var tapGesture = new TapGestureRecognizer();
            tapGesture.Tapped += (sender, args) => isFocused = true;
            GestureRecognizers.Add(tapGesture);

#if MACCATALYST
            if (UseNativeViewportHost)
            {
                var nativeViewportHost = new NativeSceneViewport();
                nativeViewportHost.HostHandleChanged += handle =>
                {
                    nativeHostHandle = handle;
                    MauiProgram.GetService<EngineService>().BindMacRemoteViewportHost(EngineService.SceneViewportId, handle);
                };

                NativeViewportContainer.Content = nativeViewportHost;
            }
#endif

            Loaded += (sender, args) =>
            {
                isRunning = true;
#if MACCATALYST
                if (!UseNativeViewportHost)
                {
                    UpdateViewportStatus();
                    return;
                }
#endif
                MauiProgram.GetService<EngineService>().RunProcess(false, "");

                Dispatcher.StartTimer(TimeSpan.FromMilliseconds(100), () =>
                {
                    if (!isRunning)
                    {
                        return false;
                    }

                    UpdateViewportIntegration();
                    UpdateViewportStatus();
                    return true;
                });
            };

            Unloaded += (sender, args) =>
            {
                isRunning = false;
                MauiProgram.GetService<EngineService>().DestroyRemoteViewport(EngineService.SceneViewportId);
            };
        }

        void UpdateViewportIntegration()
        {
            var engineService = MauiProgram.GetService<EngineService>();
            var rect = Viewport.GetAbsolutePositionWin();
#if MACCATALYST
            if (UseNativeViewportHost && nativeHostHandle != nint.Zero)
            {
                engineService.BindMacRemoteViewportHost(EngineService.SceneViewportId, nativeHostHandle);
                rect = new Rect(0, 0, Math.Max(Viewport.Width, 1), Math.Max(Viewport.Height, 1));
            }
#endif

            if (rect.IsEmpty)
            {
                ViewportStatusText.Text = "Waiting for viewport host…";
                return;
            }

            engineService.Viewport = rect;
            if (!engineService.TryUpdateRemoteViewport(EngineService.SceneViewportId, rect, visible: IsVisible, focused: isFocused))
            {
                engineService.Viewport = rect;
            }
        }

        void UpdateViewportStatus()
        {
            var engineService = MauiProgram.GetService<EngineService>();
            var state = engineService.GetRemoteViewportState(EngineService.SceneViewportId);
            var diagnostics = engineService.GetRemoteViewportDiagnostics(EngineService.SceneViewportId);

#if MACCATALYST
            if (!UseNativeViewportHost)
            {
                ViewportStatusText.Text = "MacCatalyst native viewport host temporarily disabled to avoid startup crash";
                ViewportDiagnosticsText.Text = diagnostics;
                RetryRemoteViewportButton.IsVisible = false;
                return;
            }
#endif

            ViewportStatusText.Text = state switch
            {
                RemoteViewportSessionState.Active => "Remote viewport active",
                RemoteViewportSessionState.Ready => "Remote viewport ready",
                RemoteViewportSessionState.Negotiating => "Connecting remote viewport…",
                RemoteViewportSessionState.Resizing => "Resizing remote viewport…",
                RemoteViewportSessionState.Recovering => "Remote viewport reconnecting…",
                RemoteViewportSessionState.Lost => "Remote viewport lost — using legacy embedded viewport fallback",
                RemoteViewportSessionState.Paused => "Remote viewport paused",
                RemoteViewportSessionState.Terminating => "Remote viewport terminating…",
                RemoteViewportSessionState.Disposed => "Remote viewport disposed",
                _ => "Using legacy embedded viewport fallback"
            };

            ViewportDiagnosticsText.Text = diagnostics;
            RetryRemoteViewportButton.IsVisible = state is RemoteViewportSessionState.Recovering or RemoteViewportSessionState.Lost;

            if (!string.Equals(lastViewportStatusText, ViewportStatusText.Text, StringComparison.Ordinal))
            {
                lastViewportStatusText = ViewportStatusText.Text;
                Console.WriteLine($"[SceneView] status='{ViewportStatusText.Text}' diagnostics='{diagnostics}'");
            }
        }

        void OnRetryRemoteViewportClicked(object sender, EventArgs e)
        {
            MauiProgram.GetService<EngineService>().RetryRemoteViewport(EngineService.SceneViewportId);
        }
    }
}
