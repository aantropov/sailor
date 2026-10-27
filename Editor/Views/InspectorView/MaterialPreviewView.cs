using System.ComponentModel;
using SailorEditor.Services;
using SailorEditor.ViewModels;

namespace SailorEditor.Views;

public sealed class MaterialPreviewView : ContentView
{
    readonly Image previewImage;
    readonly Label status;
    readonly Border previewBorder;
    readonly VerticalStackLayout previewContent;
    readonly Picker primitive;
    readonly Slider zoom;
    readonly Button toggle;
    readonly List<VisualElement> visibilityAncestors = [];
    EngineService? engine;
    MaterialFile? material;
    MaterialPreviewController? controller;
    byte[]? displayedPng;
    bool loaded;
    bool expanded = true;
    bool snapshotQueued;
    float yaw = 0.45f;
    float pitch = 0.25f;
    float dragYaw;
    float dragPitch;

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
        SemanticProperties.SetDescription(previewImage, "Rendered material preview. Drag to orbit.");
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

        zoom = new Slider
        {
            AutomationId = "MaterialPreviewZoom",
            Minimum = 1.5,
            Maximum = 12,
            Value = 3,
            HorizontalOptions = LayoutOptions.Fill
        };
        SemanticProperties.SetDescription(zoom, "Preview camera distance");
        zoom.ValueChanged += (_, _) => QueueSnapshot();
        var zoomControls = new Grid
        {
            ColumnDefinitions = [new ColumnDefinition(GridLength.Auto), new ColumnDefinition(GridLength.Star)],
            ColumnSpacing = 6
        };
        zoomControls.Add(new Label { Text = "Distance", VerticalOptions = LayoutOptions.Center }, 0, 0);
        zoomControls.Add(zoom, 1, 0);
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
            Children = { previewBorder, shapeControls, zoomControls, status }
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
            return;
        loaded = true;
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
        snapshotQueued = false;
        if (material is not null)
            material.PropertyChanged -= OnMaterialChanged;
        material = null;
        if (engine is not null)
            engine.OnLifecycleStateChanged -= OnEngineStateChanged;
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
            material.PropertyChanged -= OnMaterialChanged;
        material = loaded ? BindingContext as MaterialFile : null;
        if (material is not null)
            material.PropertyChanged += OnMaterialChanged;
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
            ancestor.PropertyChanged -= OnAncestorChanged;
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
            SyncContext();
    }

    bool CanPreview() => loaded && expanded && Window is not null && material is not null && engine?.IsRunning == true &&
        Width > 0 && Height > 0 && visibilityAncestors.All(parent => parent.IsVisible && parent.Width != 0 && parent.Height != 0);

    void SyncContext()
    {
        if (controller is null || engine is null)
            return;
        var active = CanPreview();
        var changed = controller.SetContext(material, engine.Generation, active);
        if (active && changed)
            QueueSnapshot();
        if (!engine.IsRunning)
            status.Text = "Start the engine to preview this material.";
    }

    void OnEngineStateChanged(EngineLifecycleState state) => SyncContext();

    void OnMaterialChanged(object? sender, PropertyChangedEventArgs args) => Dispatcher.Dispatch(QueueSnapshot);

    void QueueSnapshot()
    {
        if (snapshotQueued || !CanPreview())
            return;
        snapshotQueued = true;
        Dispatcher.Dispatch(() =>
        {
            snapshotQueued = false;
            if (!CanPreview() || controller is null || material is null)
                return;
            try
            {
                controller.Update(new MaterialPreviewSnapshot(
                    material.CreatePreviewSnapshot(),
                    (PreviewPrimitive)(Math.Max(primitive.SelectedIndex, 0) + 1),
                    yaw, pitch, (float)zoom.Value));
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
            return;
        Dispatcher.Dispatch(() =>
        {
            if (!loaded || !ReferenceEquals(source, controller) || !ReferenceEquals(state, source.Presentation))
                return;
            if (!ReferenceEquals(displayedPng, state.Png))
            {
                displayedPng = state.Png;
                var png = state.Png;
                previewImage.Source = png is null ? null : ImageSource.FromStream(() => new MemoryStream(png, writable: false));
            }
            status.Text = state.IsLoading ? "Rendering…" :
                !string.IsNullOrEmpty(state.Diagnostic) ? state.Diagnostic :
                state.Png is not null ? "Drag to orbit. Focus resets the camera." :
                "Start the engine to preview this material.";
        });
    }

    void SelectPreview()
    {
        previewBorder.Stroke = new SolidColorBrush(Color.FromArgb("#3E8EED"));
        previewBorder.StrokeThickness = 2;
        Focus();
    }

    void ResetCamera()
    {
        SelectPreview();
        yaw = 0.45f;
        pitch = 0.25f;
        zoom.Value = 3;
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
        else if (args.StatusType is GestureStatus.Running or GestureStatus.Completed)
        {
            yaw = (float)Math.IEEERemainder(dragYaw - args.TotalX * 0.012, Math.PI * 2);
            pitch = Math.Clamp(dragPitch + (float)args.TotalY * 0.012f, -1.4f, 1.4f);
            QueueSnapshot();
        }
    }
}
