#nullable enable
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Controls;
using SailorEditor.Scene;
using System.Diagnostics;
using System.Text.RegularExpressions;

namespace SailorEditor.Views
{
    public partial class SceneView : ContentView
    {
        bool isRunning = false;
        bool isFocused = false;
        bool viewportRetryQueued = false;
        long viewportRetryUntilMs = -1;
        NativeSceneViewport? nativeViewportHost;
        nint nativeHostHandle = nint.Zero;
        double nativeViewportWidth = 0;
        double nativeViewportHeight = 0;
        double nativeViewportScale = 1;
        double lastRequestedNativeViewportWidth = -1;
        double lastRequestedNativeViewportHeight = -1;
        double lastRequestedNativeViewportScale = -1;
        double lastAppliedLogicalViewportWidth = -1;
        double lastAppliedLogicalViewportHeight = -1;
        double lastAppliedNativeViewportScale = -1;
        uint lastAppliedRenderTargetWidth = 0;
        uint lastAppliedRenderTargetHeight = 0;
        string lastViewportStatusText = string.Empty;
        readonly Stopwatch uiUpdateStopwatch = Stopwatch.StartNew();
        readonly SceneViewportLifecycleAdapter viewportAdapter;
        readonly SceneShellFocusCoordinator focusCoordinator;
        long lastViewportIntegrationTickMs = -1;
        long lastViewportStatusTickMs = -1;
#if MACCATALYST
        static readonly bool UseNativeViewportHost = true;
#else
        static readonly bool UseNativeViewportHost = false;
#endif

        public SceneView()
        {
            InitializeComponent();
            viewportAdapter = new SceneViewportLifecycleAdapter(new EngineSceneViewportBackend(MauiProgram.GetService<EngineService>()), EngineService.SceneViewportId);
            focusCoordinator = new SceneShellFocusCoordinator(MauiProgram.GetService<State.ShellState>(), $"scene:{EngineService.SceneViewportId}");

            var tapGesture = new TapGestureRecognizer();
            tapGesture.Tapped += (sender, args) =>
            {
                isFocused = true;
                focusCoordinator.SetViewportFocus(true);
            };
            GestureRecognizers.Add(tapGesture);

            SizeChanged += (sender, args) =>
            {
                RequestNativeViewportLayout();
                QueueViewportRetry();
            };
            Unloaded += (sender, args) =>
            {
                isFocused = false;
                focusCoordinator.SetViewportFocus(false);
                viewportAdapter.SendInput(RemoteViewportInputKind.Focus, focused: false);
            };

#if MACCATALYST
            if (UseNativeViewportHost)
            {
                nativeViewportHost = new NativeSceneViewport
                {
                    HorizontalOptions = LayoutOptions.Fill,
                    VerticalOptions = LayoutOptions.Fill
                };
                nativeViewportHost.HostHandleChanged += handle =>
                {
                    nativeHostHandle = handle;
                    Console.WriteLine($"[SceneView] native host handle changed: 0x{handle.ToInt64():X}");
                    if (handle != nint.Zero)
                    {
                        QueueViewportRetry();
                    }
                };
                nativeViewportHost.HostLayoutChanged += (width, height, scale) =>
                {
                    nativeViewportWidth = width;
                    nativeViewportHeight = height;
                    nativeViewportScale = scale > 0 ? scale : 1;
                    Console.WriteLine($"[SceneView] native host layout: {width:0.##}x{height:0.##} scale={nativeViewportScale:0.##}");
                    QueueViewportRetry();
                    if (isRunning)
                    {
                        UpdateViewportIntegration();
                    }
                };
                nativeViewportHost.InputReceived += input =>
                {
                    if (input.Kind == NativeSceneViewportInputKind.Focus)
                    {
                        isFocused = input.Focused;
                        focusCoordinator.SetViewportFocus(isFocused);
                    }
                    else
                    {
                        isFocused = true;
                        focusCoordinator.SetViewportFocus(true);
                    }

                    viewportAdapter.SendInput(
                        (RemoteViewportInputKind)input.Kind,
                        input.PointerX,
                        input.PointerY,
                        input.WheelDeltaX,
                        input.WheelDeltaY,
                        input.KeyCode,
                        input.Button,
                        (RemoteViewportInputModifier)input.Modifiers,
                        input.Pressed,
                        input.Focused,
                        input.Captured);
                    UpdateViewportIntegration();
                };

                ViewportStatusOverlay.IsVisible = true;
                NativeViewportContainer.Content = nativeViewportHost;
                RequestNativeViewportLayout();
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
                RequestNativeViewportLayout();

                Dispatcher.StartTimer(TimeSpan.FromMilliseconds(50), () =>
                {
                    try
                    {
                        if (!isRunning)
                        {
                            return false;
                        }

                        var nowMs = uiUpdateStopwatch.ElapsedMilliseconds;
                        if (lastViewportIntegrationTickMs < 0 || nowMs - lastViewportIntegrationTickMs >= 50)
                        {
                            lastViewportIntegrationTickMs = nowMs;
                            UpdateViewportIntegration();
                        }

                        if (lastViewportStatusTickMs < 0 || nowMs - lastViewportStatusTickMs >= 500)
                        {
                            lastViewportStatusTickMs = nowMs;
                            UpdateViewportStatus();
                        }
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"[SceneView] viewport timer exception: {ex}");
                    }

                    return true;
                });
            };

            Unloaded += (sender, args) =>
            {
                isRunning = false;
                focusCoordinator.ReleaseIfOwned();
                viewportAdapter.Destroy();
            };
        }

        void UpdateViewportIntegration()
        {
            var remoteRect = Viewport.GetAbsolutePositionWin();
            var editorRect = new SceneViewportRect(remoteRect.X, remoteRect.Y, remoteRect.Width, remoteRect.Height);
            var renderTarget = default(SceneViewportRenderTarget);
#if MACCATALYST
            if (UseNativeViewportHost)
            {
                if (nativeHostHandle == nint.Zero)
                {
                    if (ViewportStatusOverlay.IsVisible)
                        SetLabelText(ViewportStatusText, "Waiting for native CAMetalLayer host…");
                    return;
                }

                var logicalViewportWidth = nativeViewportWidth > 1 ? nativeViewportWidth : NativeViewportContainer.Width;
                var logicalViewportHeight = nativeViewportHeight > 1 ? nativeViewportHeight : NativeViewportContainer.Height;
                if (logicalViewportWidth <= 1 || logicalViewportHeight <= 1)
                {
                    if (ViewportStatusOverlay.IsVisible)
                        SetLabelText(ViewportStatusText, $"Waiting for native viewport layout… host={nativeHostHandle != nint.Zero} size={logicalViewportWidth:0.##}x{logicalViewportHeight:0.##}");
                    return;
                }

                var renderWidth = (uint)Math.Max(1, Math.Round(logicalViewportWidth * nativeViewportScale));
                var renderHeight = (uint)Math.Max(1, Math.Round(logicalViewportHeight * nativeViewportScale));
                if (Math.Abs(lastAppliedLogicalViewportWidth - logicalViewportWidth) >= 0.5 ||
                    Math.Abs(lastAppliedLogicalViewportHeight - logicalViewportHeight) >= 0.5 ||
                    Math.Abs(lastAppliedNativeViewportScale - nativeViewportScale) >= 0.01 ||
                    lastAppliedRenderTargetWidth != renderWidth ||
                    lastAppliedRenderTargetHeight != renderHeight)
                {
                    lastAppliedLogicalViewportWidth = logicalViewportWidth;
                    lastAppliedLogicalViewportHeight = logicalViewportHeight;
                    lastAppliedNativeViewportScale = nativeViewportScale;
                    lastAppliedRenderTargetWidth = renderWidth;
                    lastAppliedRenderTargetHeight = renderHeight;
                    Console.WriteLine($"[SceneView] viewport sizes: logical={logicalViewportWidth:0.##}x{logicalViewportHeight:0.##} scale={nativeViewportScale:0.##} render={renderWidth}x{renderHeight}");
                    QueueViewportRetry(TimeSpan.FromMilliseconds(350));
                }

                editorRect = new SceneViewportRect(0, 0, Math.Max(1, logicalViewportWidth), Math.Max(1, logicalViewportHeight));
                remoteRect = new Rect(0, 0, renderWidth, renderHeight);
                renderTarget = new SceneViewportRenderTarget(renderWidth, renderHeight);
            }
#endif

            if (remoteRect.IsEmpty)
            {
                if (ViewportStatusOverlay.IsVisible)
                    SetLabelText(ViewportStatusText, "Waiting for viewport host…");
                return;
            }

            var updated = viewportAdapter.Sync(new SceneViewportFrame(
                new SceneViewportRect(remoteRect.X, remoteRect.Y, remoteRect.Width, remoteRect.Height),
                editorRect,
                renderTarget,
                IsVisible,
                isFocused,
                nativeHostHandle));

            if (!updated)
                viewportAdapter.Sync(new SceneViewportFrame(editorRect, editorRect, renderTarget, IsVisible, isFocused, nativeHostHandle));
        }

        void UpdateViewportStatus()
        {
            var state = viewportAdapter.GetState();
            var diagnostics = viewportAdapter.GetDiagnostics();

#if MACCATALYST
            if (!UseNativeViewportHost)
            {
                SetLabelText(ViewportStatusText, "MacCatalyst native viewport host temporarily disabled to avoid startup crash");
                SetLabelText(ViewportTruthText, "Truth: native host disabled");
                ViewportTruthText.TextColor = Color.FromArgb("#FF6B6B");
                SetLabelText(ViewportSourceText, "Source: unavailable");
                SetLabelText(ViewportEvidenceText, "Evidence: unavailable");
                SetLabelText(ViewportGeometryText, "Viewport: unavailable");
                SetLabelText(ViewportDiagnosticsText, diagnostics);
                RetryRemoteViewportButton.IsVisible = false;
                return;
            }
#endif

            SetLabelText(ViewportStatusText, state switch
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
            });

            if (!string.Equals(lastViewportStatusText, ViewportStatusText.Text, StringComparison.Ordinal))
            {
                lastViewportStatusText = ViewportStatusText.Text;
                Console.WriteLine($"[SceneView] status='{ViewportStatusText.Text}' diagnostics='{diagnostics}'");
            }

#if MACCATALYST
            if (!ViewportStatusOverlay.IsVisible)
            {
                RetryRemoteViewportButton.IsVisible = false;
                return;
            }
#endif

            var parsed = ParseViewportDiagnostics(diagnostics);
            ApplyViewportDiagnosticSummary(parsed, state, diagnostics);
            RetryRemoteViewportButton.IsVisible = state is RemoteViewportSessionState.Recovering or RemoteViewportSessionState.Lost;
        }

        void RequestNativeViewportLayout()
        {
#if MACCATALYST
            if (!UseNativeViewportHost || nativeViewportHost is null)
            {
                return;
            }

            var width = NativeViewportContainer.Width > 1 ? NativeViewportContainer.Width : Viewport.Width;
            var height = NativeViewportContainer.Height > 1 ? NativeViewportContainer.Height : Viewport.Height;
            if (width <= 1 || height <= 1)
            {
                return;
            }

            var scale = nativeViewportScale > 0 ? nativeViewportScale : 1;
            if (Math.Abs(lastRequestedNativeViewportWidth - width) < 0.5 &&
                Math.Abs(lastRequestedNativeViewportHeight - height) < 0.5 &&
                Math.Abs(lastRequestedNativeViewportScale - scale) < 0.01)
            {
                return;
            }

            lastRequestedNativeViewportWidth = width;
            lastRequestedNativeViewportHeight = height;
            lastRequestedNativeViewportScale = scale;
            nativeViewportHost.RequestLayoutUpdate(width, height, scale);
#endif
        }

        void QueueViewportRetry(TimeSpan? keepAlive = null)
        {
#if MACCATALYST
            if (!UseNativeViewportHost || !isRunning)
            {
                return;
            }

            if (keepAlive.HasValue)
            {
                viewportRetryUntilMs = Math.Max(viewportRetryUntilMs, uiUpdateStopwatch.ElapsedMilliseconds + (long)keepAlive.Value.TotalMilliseconds);
            }

            if (viewportRetryQueued)
            {
                return;
            }

            viewportRetryQueued = true;
            Dispatcher.DispatchDelayed(TimeSpan.FromMilliseconds(33), () =>
            {
                viewportRetryQueued = false;
                if (!isRunning || nativeHostHandle == nint.Zero)
                {
                    return;
                }

                var width = NativeViewportContainer.Width > 1 ? NativeViewportContainer.Width : Viewport.Width;
                var height = NativeViewportContainer.Height > 1 ? NativeViewportContainer.Height : Viewport.Height;
                if (width <= 1 || height <= 1)
                {
                    return;
                }

                nativeViewportHost?.RequestLayoutUpdate(width, height, nativeViewportScale);
                UpdateViewportIntegration();
                viewportAdapter.Retry();

                if (uiUpdateStopwatch.ElapsedMilliseconds < viewportRetryUntilMs)
                {
                    QueueViewportRetry();
                }
            });
#endif
        }

        void OnRetryRemoteViewportClicked(object sender, EventArgs e)
        {
            viewportAdapter.Retry();
        }

        sealed class ParsedViewportDiagnostics
        {
            public string SourceName { get; set; } = "unknown";
            public string SourceKind { get; set; } = "unknown";
            public bool? SyntheticSource { get; set; }
            public bool? NativeLayer { get; set; }
            public bool? Readable { get; set; }
            public bool? NonBlack { get; set; }
            public bool? Variance { get; set; }
            public string Size { get; set; } = "unknown";
            public string SourceSize { get; set; } = "unknown";
            public string SourcePitch { get; set; } = "unknown";
            public string PresentCount { get; set; } = "0";
            public string LastGoodFrame { get; set; } = "0";
            public string Event { get; set; } = string.Empty;
            public string DrawableToken { get; set; } = "0";
            public string CopyToken { get; set; } = "0";
            public string AvgLuma { get; set; } = "?";
            public string MaxLuma { get; set; } = "?";
            public string NonBlackPct { get; set; } = "?";
            public string Checksum { get; set; } = "?";
            public string TopLeftSample { get; set; } = "?";
            public string CenterSample { get; set; } = "?";
            public string BottomRightSample { get; set; } = "?";
            public string ProbeSummary { get; set; } = string.Empty;
        }

        static ParsedViewportDiagnostics ParseViewportDiagnostics(string diagnostics)
        {
            var parsed = new ParsedViewportDiagnostics();
            if (string.IsNullOrWhiteSpace(diagnostics))
            {
                return parsed;
            }

            parsed.SourceName = MatchValue(diagnostics, "sourceName") ?? parsed.SourceName;
            parsed.SourceKind = MatchValue(diagnostics, "sourceKind") ?? parsed.SourceKind;
            parsed.SyntheticSource = MatchBool(diagnostics, "syntheticSource");
            parsed.NativeLayer = MatchBool(diagnostics, "nativeLayer");
            parsed.Readable = MatchBool(diagnostics, "readable");
            parsed.NonBlack = MatchBool(diagnostics, "nonBlack");
            parsed.Variance = MatchBool(diagnostics, "variance");
            parsed.Size = MatchValue(diagnostics, "size") ?? parsed.Size;
            parsed.SourceSize = MatchValue(diagnostics, "srcSize") ?? parsed.SourceSize;
            parsed.SourcePitch = MatchValue(diagnostics, "srcPitch") ?? parsed.SourcePitch;
            parsed.PresentCount = MatchValue(diagnostics, "presentCount") ?? parsed.PresentCount;
            parsed.LastGoodFrame = MatchValue(diagnostics, "lastGoodFrame") ?? parsed.LastGoodFrame;
            parsed.Event = MatchValue(diagnostics, "event") ?? string.Empty;
            parsed.DrawableToken = MatchValue(diagnostics, "drawableToken") ?? parsed.DrawableToken;
            parsed.CopyToken = MatchValue(diagnostics, "copyToken") ?? parsed.CopyToken;
            parsed.AvgLuma = MatchValue(diagnostics, "avgLuma") ?? parsed.AvgLuma;
            parsed.MaxLuma = MatchValue(diagnostics, "maxLuma") ?? parsed.MaxLuma;
            parsed.NonBlackPct = MatchValue(diagnostics, "nonBlackPct") ?? parsed.NonBlackPct;
            parsed.Checksum = MatchValue(diagnostics, "checksum") ?? parsed.Checksum;
            parsed.TopLeftSample = MatchValue(diagnostics, "tl") ?? parsed.TopLeftSample;
            parsed.CenterSample = MatchValue(diagnostics, "c") ?? parsed.CenterSample;
            parsed.BottomRightSample = MatchValue(diagnostics, "br") ?? parsed.BottomRightSample;
            parsed.ProbeSummary = MatchProbeSummary(diagnostics);
            return parsed;
        }

        void ApplyViewportDiagnosticSummary(ParsedViewportDiagnostics parsed, RemoteViewportSessionState state, string diagnostics)
        {
            var seesRealSource = parsed.SyntheticSource == false;
            var seesSceneEvidence = parsed.Readable == true && (parsed.NonBlack == true || parsed.Variance == true);
            var onlyBlackEvidence = parsed.Readable == true && parsed.NonBlack == false && parsed.Variance == false;

            if (state == RemoteViewportSessionState.Active && seesRealSource && seesSceneEvidence)
            {
                SetLabelText(ViewportTruthText, "Truth: REAL renderer source with meaningful scene pixels");
                ViewportTruthText.TextColor = Color.FromArgb("#7DFFB3");
            }
            else if (state == RemoteViewportSessionState.Active && parsed.SyntheticSource == true)
            {
                SetLabelText(ViewportTruthText, "Truth: ACTIVE but still showing synthetic source");
                ViewportTruthText.TextColor = Color.FromArgb("#FFB86B");
            }
            else if (state == RemoteViewportSessionState.Active && onlyBlackEvidence)
            {
                SetLabelText(ViewportTruthText, "Truth: ACTIVE but frame samples still read as black/static");
                ViewportTruthText.TextColor = Color.FromArgb("#FFD166");
            }
            else
            {
                SetLabelText(ViewportTruthText, "Truth: not enough proof yet from the current feed");
                ViewportTruthText.TextColor = Color.FromArgb("#FF6B6B");
            }

            var sourceFlavor = parsed.SyntheticSource switch
            {
                true => "synthetic",
                false => "real",
                _ => "unknown"
            };

            SetLabelText(ViewportSourceText, $"Source: {parsed.SourceName} [{parsed.SourceKind}] / {sourceFlavor} copyToken={parsed.CopyToken}");
            SetLabelText(ViewportEvidenceText, $"Evidence: readable={FormatBool(parsed.Readable)} nonBlack={FormatBool(parsed.NonBlack)} variance={FormatBool(parsed.Variance)} avgLuma={parsed.AvgLuma} maxLuma={parsed.MaxLuma} nonBlackPct={parsed.NonBlackPct} checksum={parsed.Checksum} lastGoodFrame={parsed.LastGoodFrame} event={DefaultText(parsed.Event, "n/a")}");
            SetLabelText(ViewportGeometryText, $"Viewport: size={parsed.Size} sourceSize={parsed.SourceSize} srcPitch={parsed.SourcePitch} presents={parsed.PresentCount} nativeLayer={FormatBool(parsed.NativeLayer)} drawable={parsed.DrawableToken}");
            SetLabelText(ViewportSamplesText, $"Samples: tl={parsed.TopLeftSample} c={parsed.CenterSample} br={parsed.BottomRightSample}");
            SetLabelText(ViewportProbeText, $"Probe: {DefaultText(parsed.ProbeSummary, "n/a")}");
#if MACCATALYST
            if (state is RemoteViewportSessionState.Active or RemoteViewportSessionState.Ready or RemoteViewportSessionState.Recovering or RemoteViewportSessionState.Lost)
            {
                SetLabelText(ViewportDiagnosticsText, diagnostics);
            }
#else
            SetLabelText(ViewportDiagnosticsText, diagnostics);
#endif
        }

        static string? MatchValue(string diagnostics, string key)
        {
            var quoted = Regex.Match(diagnostics, $@"\b{Regex.Escape(key)}='([^']*)'");
            if (quoted.Success)
            {
                return quoted.Groups[1].Value;
            }

            var plain = Regex.Match(diagnostics, $@"\b{Regex.Escape(key)}=([^\s\]]+)");
            return plain.Success ? plain.Groups[1].Value : null;
        }

        static bool? MatchBool(string diagnostics, string key)
        {
            var value = MatchValue(diagnostics, key);
            return value switch
            {
                "1" => true,
                "0" => false,
                _ => null
            };
        }

        static string MatchProbeSummary(string diagnostics)
        {
            var probe = Regex.Match(diagnostics, @"probe\{(.*)\}");
            return probe.Success ? probe.Groups[1].Value.Trim() : string.Empty;
        }

        static void SetLabelText(Label label, string text)
        {
            if (!string.Equals(label.Text, text, StringComparison.Ordinal))
            {
                label.Text = text;
            }
        }

        static string FormatBool(bool? value) => value.HasValue ? (value.Value ? "yes" : "no") : "?";

        static string DefaultText(string? value, string fallback) => string.IsNullOrWhiteSpace(value) ? fallback : value;
    }
}
