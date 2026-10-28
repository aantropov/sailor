using System.ComponentModel;
using SailorEditor.Controls;
using SailorEditor.Scene;
using SailorEditor.Services;
using SailorEditor.State;
using SailorEditor.ViewModels;

namespace SailorEditor.Views;

public sealed class MaterialPreviewView : ContentView
{
    readonly Image previewImage;
    readonly Label status;
    readonly Border previewBorder;
    readonly VerticalStackLayout previewContent;
    readonly Picker primitive;
    readonly Button toggle;
    readonly List<VisualElement> visibilityAncestors = [];
    EngineService? engine;
    MaterialFile? material;
    MaterialPreviewController? controller;
    ShellState? shellState;
    RenderViewFocusCoordinator? focusCoordinator;
    readonly string viewportId = $"material-preview:{Guid.NewGuid():N}";
    string? materialYaml;
    byte[]? displayedPng;
    bool loaded;
    bool expanded = true;
    bool snapshotQueued;
    float yaw = 0.45f;
    float pitch = 0.25f;
    float dragYaw;
    float dragPitch;
    float distance = 3;
    internal int ReceivedWheelEvents { get; private set; }

    public MaterialPreviewView()
    {
        AutomationId = "MaterialPreview";
        previewImage = new Image
        {
            AutomationId = "MaterialPreviewImage",
            Aspect = Aspect.AspectFit,
            HeightRequest = 224,
            HorizontalOptions = LayoutOptions.Fill
        };
        SemanticProperties.SetDescription(previewImage, "Rendered material preview. Drag to orbit, scroll to zoom.");
        previewBorder = new Border
        {
            Content = previewImage,
            Stroke = new SolidColorBrush(Color.FromArgb("#606060")),
            StrokeThickness = 1,
            BackgroundColor = Color.FromArgb("#25282D")
        };
        var tap = new TapGestureRecognizer();
        tap.Tapped += (_, _) => SelectPreview();
        previewBorder.GestureRecognizers.Add(tap);
        var pan = new PanGestureRecognizer();
        pan.PanUpdated += OnPanUpdated;
        previewBorder.GestureRecognizers.Add(pan);
        previewBorder.Behaviors.Add(new PreviewWheelBehavior(OnPreviewWheel));

        primitive = new Picker
        {
            AutomationId = "MaterialPreviewPrimitive",
            Title = "Preview shape",
            ItemsSource = new[] { "Sphere", "Plane", "Box" },
            SelectedIndex = 0,
            HorizontalOptions = LayoutOptions.Fill
        };
        primitive.SelectedIndexChanged += (_, _) => QueueSnapshot();
        var focus = new Button { AutomationId = "MaterialPreviewFocus", Text = "Focus", Padding = new Thickness(8, 3) };
        focus.Clicked += (_, _) => ResetCamera();
        var shapeControls = new Grid
        {
            ColumnDefinitions = [new ColumnDefinition(GridLength.Star), new ColumnDefinition(GridLength.Auto)],
            ColumnSpacing = 6
        };
        shapeControls.Add(primitive, 0, 0);
        shapeControls.Add(focus, 1, 0);

        status = new Label
        {
            AutomationId = "MaterialPreviewStatus",
            Text = "Start the engine to preview this material.",
            FontSize = 11,
            LineBreakMode = LineBreakMode.WordWrap
        };
        previewContent = new VerticalStackLayout
        {
            Spacing = 4,
            Children = { previewBorder, shapeControls, status }
        };
        toggle = new Button { AutomationId = "MaterialPreviewToggle", Text = "Hide", Padding = new Thickness(8, 3) };
        toggle.Clicked += (_, _) =>
        {
            expanded = !expanded;
            previewContent.IsVisible = expanded;
            toggle.Text = expanded ? "Hide" : "Show";
            SyncContext();
        };
        var heading = new Grid
        {
            ColumnDefinitions = [new ColumnDefinition(GridLength.Star), new ColumnDefinition(GridLength.Auto)]
        };
        heading.Add(new Label { Text = "Material preview", FontAttributes = FontAttributes.Bold, VerticalOptions = LayoutOptions.Center }, 0, 0);
        heading.Add(toggle, 1, 0);
        Content = new VerticalStackLayout { Spacing = 6, Margin = new Thickness(0, 8), Children = { heading, previewContent } };
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += (_, _) => SyncContext();
        BindingContextChanged += (_, _) => BindMaterial();
    }

    void OnLoaded(object? sender, EventArgs args)
    {
        if (loaded)
        {
            return;
        }
        loaded = true;
        shellState = MauiProgram.GetService<ShellState>();
        focusCoordinator = new RenderViewFocusCoordinator(shellState, viewportId);
        shellState.PropertyChanged += OnShellFocusChanged;
        engine = MauiProgram.GetService<EngineService>();
        engine.OnLifecycleStateChanged += OnEngineStateChanged;
        controller = new MaterialPreviewController(engine);
        controller.Changed += OnPreviewChanged;
        ObserveVisibility();
        BindMaterial();
    }

    void OnUnloaded(object? sender, EventArgs args)
    {
        loaded = false;
        focusCoordinator?.ReleaseIfOwned();
        if (shellState is not null)
        {
            shellState.PropertyChanged -= OnShellFocusChanged;
        }
        focusCoordinator = null;
        shellState = null;
        UpdateFocusVisual();
        snapshotQueued = false;
        if (material is not null)
        {
            material.PropertyChanged -= OnMaterialChanged;
        }
        material = null;
        materialYaml = null;
        if (engine is not null)
        {
            engine.OnLifecycleStateChanged -= OnEngineStateChanged;
        }
        StopObservingVisibility();
        if (controller is not null)
        {
            controller.Changed -= OnPreviewChanged;
            _ = controller.DisposeAsync();
            controller = null;
        }
        displayedPng = null;
        previewImage.Source = null;
    }

    void BindMaterial()
    {
        if (material is not null)
        {
            material.PropertyChanged -= OnMaterialChanged;
        }
        material = loaded ? BindingContext as MaterialFile : null;
        materialYaml = null;
        if (material is not null)
        {
            material.PropertyChanged += OnMaterialChanged;
        }
        SyncContext();
    }

    void ObserveVisibility()
    {
        StopObservingVisibility();
        for (Element? parent = this; parent is not null; parent = parent.Parent)
        {
            if (parent is VisualElement visual)
            {
                visibilityAncestors.Add(visual);
                visual.PropertyChanged += OnAncestorChanged;
            }
        }
    }

    void StopObservingVisibility()
    {
        foreach (var ancestor in visibilityAncestors)
        {
            ancestor.PropertyChanged -= OnAncestorChanged;
        }
        visibilityAncestors.Clear();
    }

    void OnAncestorChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (args.PropertyName == nameof(Parent))
        {
            ObserveVisibility();
            SyncContext();
        }
        else if (args.PropertyName is nameof(IsVisible) or nameof(Width) or nameof(Height))
        {
            SyncContext();
        }
    }

    bool CanPreview() => loaded && expanded && Window is not null && material is not null && engine?.IsRunning == true &&
        Width > 0 && Height > 0 && visibilityAncestors.All(parent => parent.IsVisible && parent.Width != 0 && parent.Height != 0);

    void SyncContext()
    {
        if (controller is null || engine is null)
        {
            return;
        }
        var active = CanPreview();
        if (!active)
        {
            focusCoordinator?.ReleaseIfOwned();
        }
        var changed = controller.SetContext(material, engine.Generation, active);
        if (active && changed)
        {
            QueueSnapshot();
        }
        if (!engine.IsRunning)
        {
            status.Text = "Start the engine to preview this material.";
        }
    }

    void OnEngineStateChanged(EngineLifecycleState state) => SyncContext();

    void OnMaterialChanged(object? sender, PropertyChangedEventArgs args) => Dispatcher.Dispatch(() =>
    {
        materialYaml = null;
        QueueSnapshot();
    });

    void QueueSnapshot()
    {
        if (snapshotQueued || !CanPreview())
        {
            return;
        }
        snapshotQueued = true;
        Dispatcher.Dispatch(() =>
        {
            snapshotQueued = false;
            if (!CanPreview() || controller is null || material is null)
            {
                return;
            }
            try
            {
                controller.Update(new MaterialPreviewSnapshot(
                    materialYaml ??= material.CreatePreviewSnapshot(),
                    (PreviewPrimitive)(Math.Max(primitive.SelectedIndex, 0) + 1),
                    yaw, pitch, distance));
            }
            catch (Exception exception)
            {
                controller.ReportSnapshotError(exception.Message);
            }
        });
    }

    void OnPreviewChanged()
    {
        var source = controller;
        var state = source?.Presentation;
        if (source is null || state is null)
        {
            return;
        }
        Dispatcher.Dispatch(() =>
        {
            if (!loaded || !ReferenceEquals(source, controller) || !ReferenceEquals(state, source.Presentation))
            {
                return;
            }
            if (!ReferenceEquals(displayedPng, state.Png))
            {
                displayedPng = state.Png;
                var png = state.Png;
                previewImage.Source = png is null ? null : ImageSource.FromStream(() => new MemoryStream(png, writable: false));
            }
            status.Text = state.IsLoading ? "Rendering…" :
                !string.IsNullOrEmpty(state.Diagnostic) ? state.Diagnostic :
                state.Png is not null ? "Drag to orbit. Scroll to zoom. Focus resets the camera." :
                "Start the engine to preview this material.";
        });
    }

    void SelectPreview()
    {
        focusCoordinator?.SetViewportFocus(true);
        UpdateFocusVisual();
        Focus();
    }

    void OnShellFocusChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (args.PropertyName == nameof(ShellState.Focus))
        {
            UpdateFocusVisual();
        }
    }

    void UpdateFocusVisual()
    {
        var focused = focusCoordinator?.IsFocused == true;
        previewBorder.Stroke = new SolidColorBrush(Color.FromArgb(focused ? "#3E8EED" : "#606060"));
        previewBorder.StrokeThickness = focused ? 2 : 1;
    }

    void OnPreviewWheel(double delta)
    {
        if (!CanPreview() || !double.IsFinite(delta) || delta == 0)
        {
            return;
        }
        ++ReceivedWheelEvents;
        SelectPreview();
        distance = Math.Clamp(distance * (float)Math.Exp(-delta * 0.12), 1.5f, 12f);
        QueueSnapshot();
    }

    void ResetCamera()
    {
        SelectPreview();
        yaw = 0.45f;
        pitch = 0.25f;
        distance = 3;
        QueueSnapshot();
    }

    void OnPanUpdated(object? sender, PanUpdatedEventArgs args)
    {
        if (args.StatusType == GestureStatus.Started)
        {
            SelectPreview();
            dragYaw = yaw;
            dragPitch = pitch;
        }
        else if (args.StatusType == GestureStatus.Running)
        {
            yaw = (float)Math.IEEERemainder(dragYaw - args.TotalX * 0.012, Math.PI * 2);
            pitch = Math.Clamp(dragPitch + (float)args.TotalY * 0.012f, -1.4f, 1.4f);
            QueueSnapshot();
        }
        else if (args.StatusType == GestureStatus.Completed)
        {
            QueueSnapshot();
        }
    }
}
