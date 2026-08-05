using SailorEditor.ViewModels;
using SailorEditor.Utility;
using System.Collections.Specialized;
using System.ComponentModel;

namespace SailorEditor.Controls;

public sealed class AnimationControllerGraphView : GraphicsView, IDrawable
{
    const float NodeWidth = 150.0f;
    const float NodeHeight = 58.0f;
    const float Padding = 8.0f;

    AnimationControllerFile? controller;
    AnimationControllerFile? subscribedController;
    ObservableList<AnimationControllerState>? subscribedStates;
    ObservableList<AnimationControllerTransition>? subscribedTransitions;
    AnimationControllerState? draggedState;
    PointF dragOffset;
    string? dragBefore;

    public AnimationControllerGraphView()
    {
        Drawable = this;
        BackgroundColor = Color.FromArgb("#171A1F");
        StartInteraction += OnStartInteraction;
        DragInteraction += OnDragInteraction;
        EndInteraction += OnEndInteraction;
        BindingContextChanged += (_, _) => BindController(BindingContext as AnimationControllerFile);
    }

    public event Action<AnimationControllerState?>? StateSelected;
    public event Action<string, string, string>? DocumentEdited;

    public AnimationControllerState? SelectedState { get; private set; }
    public ulong PreviewActiveStateId { get; set; }

    public void SelectState(AnimationControllerState? state)
    {
        SelectedState = state;
        StateSelected?.Invoke(state);
        Invalidate();
    }

    public void Detach()
    {
        Unsubscribe();
        controller = null;
        BindingContext = null;
    }

    public void Draw(ICanvas canvas, RectF dirtyRect)
    {
        canvas.FillColor = Color.FromArgb("#171A1F");
        canvas.FillRectangle(dirtyRect);
        DrawGrid(canvas, dirtyRect);
        if (controller is null)
        {
            return;
        }

        foreach (var transition in controller.Transitions)
        {
            var from = controller.States.FirstOrDefault(state => state.Id == transition.FromStateId);
            var to = controller.States.FirstOrDefault(state => state.Id == transition.ToStateId);
            if (from is null || to is null)
            {
                continue;
            }
            DrawTransition(canvas, from, to);
        }

        foreach (var state in controller.States)
        {
            DrawState(canvas, state);
        }
    }

    void BindController(AnimationControllerFile? value)
    {
        if (ReferenceEquals(controller, value))
        {
            return;
        }
        Unsubscribe();
        controller = value;
        SelectedState = null;
        Subscribe(controller);
        Invalidate();
    }

    void Subscribe(AnimationControllerFile? value)
    {
        if (value is null)
        {
            return;
        }
        subscribedController = value;
        subscribedStates = value.States;
        subscribedTransitions = value.Transitions;
        subscribedController.DocumentReplaced += OnDocumentReplaced;
        subscribedStates.CollectionChanged += OnCollectionChanged;
        subscribedTransitions.CollectionChanged += OnCollectionChanged;
        foreach (var state in subscribedStates)
        {
            state.PropertyChanged += OnItemChanged;
        }
        foreach (var transition in subscribedTransitions)
        {
            transition.PropertyChanged += OnItemChanged;
        }
    }

    void Unsubscribe()
    {
        if (subscribedController is null ||
            subscribedStates is null ||
            subscribedTransitions is null)
        {
            return;
        }
        subscribedController.DocumentReplaced -= OnDocumentReplaced;
        subscribedStates.CollectionChanged -= OnCollectionChanged;
        subscribedTransitions.CollectionChanged -= OnCollectionChanged;
        foreach (var state in subscribedStates)
        {
            state.PropertyChanged -= OnItemChanged;
        }
        foreach (var transition in subscribedTransitions)
        {
            transition.PropertyChanged -= OnItemChanged;
        }
        subscribedController = null;
        subscribedStates = null;
        subscribedTransitions = null;
    }

    void OnDocumentReplaced()
    {
        Unsubscribe();
        Subscribe(controller);
        if (SelectedState is not null)
        {
            SelectedState = controller?.States.FirstOrDefault(state => state.Id == SelectedState.Id);
            StateSelected?.Invoke(SelectedState);
        }
        Invalidate();
    }

    void OnCollectionChanged(object? sender, NotifyCollectionChangedEventArgs args)
    {
        if (args.OldItems is not null)
        {
            foreach (INotifyPropertyChanged item in args.OldItems)
            {
                item.PropertyChanged -= OnItemChanged;
            }
        }
        if (args.NewItems is not null)
        {
            foreach (INotifyPropertyChanged item in args.NewItems)
            {
                item.PropertyChanged += OnItemChanged;
            }
        }
        Invalidate();
    }

    void OnItemChanged(object? sender, PropertyChangedEventArgs args) => Invalidate();

    void OnStartInteraction(object? sender, TouchEventArgs args)
    {
        if (controller is null || args.Touches.Length == 0)
        {
            return;
        }
        var point = args.Touches[0];
        draggedState = controller.States.LastOrDefault(state => StateRect(state).Contains(point));
        SelectState(draggedState);
        if (draggedState is null)
        {
            return;
        }
        dragOffset = new PointF(point.X - draggedState.EditorX, point.Y - draggedState.EditorY);
        dragBefore = controller.CaptureDocument();
    }

    void OnDragInteraction(object? sender, TouchEventArgs args)
    {
        if (controller is null || draggedState is null || args.Touches.Length == 0)
        {
            return;
        }
        var point = args.Touches[0];
        draggedState.EditorX = Math.Max(Padding, point.X - dragOffset.X);
        draggedState.EditorY = Math.Max(Padding, point.Y - dragOffset.Y);
        Invalidate();
    }

    void OnEndInteraction(object? sender, TouchEventArgs args)
    {
        if (controller is not null && draggedState is not null && dragBefore is not null)
        {
            var after = controller.CaptureDocument();
            if (!string.Equals(dragBefore, after, StringComparison.Ordinal))
            {
                DocumentEdited?.Invoke(dragBefore, after, "Move animation state");
            }
        }
        draggedState = null;
        dragBefore = null;
    }

    void DrawGrid(ICanvas canvas, RectF bounds)
    {
        canvas.StrokeColor = Color.FromArgb("#252A32");
        canvas.StrokeSize = 1.0f;
        const float grid = 24.0f;
        for (var x = 0.0f; x < bounds.Right; x += grid)
        {
            canvas.DrawLine(x, bounds.Top, x, bounds.Bottom);
        }
        for (var y = 0.0f; y < bounds.Bottom; y += grid)
        {
            canvas.DrawLine(bounds.Left, y, bounds.Right, y);
        }
    }

    void DrawTransition(
        ICanvas canvas,
        AnimationControllerState from,
        AnimationControllerState to)
    {
        var start = new PointF(from.EditorX + NodeWidth * 0.5f, from.EditorY + NodeHeight * 0.5f);
        var end = new PointF(to.EditorX + NodeWidth * 0.5f, to.EditorY + NodeHeight * 0.5f);
        var delta = new PointF(end.X - start.X, end.Y - start.Y);
        var length = MathF.Sqrt(delta.X * delta.X + delta.Y * delta.Y);
        if (length < 1.0f)
        {
            return;
        }
        var direction = new PointF(delta.X / length, delta.Y / length);
        var normal = new PointF(-direction.Y, direction.X);
        start = new PointF(start.X + direction.X * NodeWidth * 0.45f, start.Y + direction.Y * NodeHeight * 0.45f);
        end = new PointF(end.X - direction.X * NodeWidth * 0.45f, end.Y - direction.Y * NodeHeight * 0.45f);

        canvas.StrokeColor = Color.FromArgb("#8C98A8");
        canvas.StrokeSize = 2.0f;
        canvas.DrawLine(start, end);
        const float arrow = 8.0f;
        canvas.DrawLine(
            end,
            new PointF(
                end.X - direction.X * arrow + normal.X * arrow * 0.55f,
                end.Y - direction.Y * arrow + normal.Y * arrow * 0.55f));
        canvas.DrawLine(
            end,
            new PointF(
                end.X - direction.X * arrow - normal.X * arrow * 0.55f,
                end.Y - direction.Y * arrow - normal.Y * arrow * 0.55f));
    }

    void DrawState(ICanvas canvas, AnimationControllerState state)
    {
        var rect = StateRect(state);
        var isDefault = controller?.DefaultStateId == state.Id;
        var isSelected = ReferenceEquals(SelectedState, state);
        var isActive = PreviewActiveStateId == state.Id;
        canvas.FillColor = isActive
            ? Color.FromArgb("#365E46")
            : isSelected
                ? Color.FromArgb("#344A67")
                : Color.FromArgb("#252B34");
        canvas.FillRoundedRectangle(rect, 6.0f);
        canvas.StrokeColor = isDefault
            ? Color.FromArgb("#E0B45A")
            : isSelected
                ? Color.FromArgb("#74A5E8")
                : Color.FromArgb("#56606E");
        canvas.StrokeSize = isDefault || isSelected ? 2.0f : 1.0f;
        canvas.DrawRoundedRectangle(rect, 6.0f);

        canvas.FontColor = Colors.White;
        canvas.FontSize = 13.0f;
        canvas.DrawString(
            state.Name ?? string.Empty,
            rect.X + 8.0f,
            rect.Y + 6.0f,
            rect.Width - 16.0f,
            20.0f,
            HorizontalAlignment.Left,
            VerticalAlignment.Center);
        canvas.FontColor = Color.FromArgb("#AAB2BE");
        canvas.FontSize = 11.0f;
        canvas.DrawString(
            state.Clip ?? string.Empty,
            rect.X + 8.0f,
            rect.Y + 30.0f,
            rect.Width - 16.0f,
            18.0f,
            HorizontalAlignment.Left,
            VerticalAlignment.Center);
    }

    static RectF StateRect(AnimationControllerState state) =>
        new(state.EditorX, state.EditorY, NodeWidth, NodeHeight);
}
