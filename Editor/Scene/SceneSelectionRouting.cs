using SailorEditor.Controls;
using SailorEditor.Services;

namespace SailorEditor.Scene;

public enum SceneSelectionPickStatus
{
    Unavailable = 0,
    Hit,
    Miss,
}

public readonly record struct SceneSelectionPickResult(SceneSelectionPickStatus Status, string? SelectedId = null)
{
    public static SceneSelectionPickResult Unavailable() => new(SceneSelectionPickStatus.Unavailable);
    public static SceneSelectionPickResult Hit(string selectedId) => new(SceneSelectionPickStatus.Hit, selectedId);
    public static SceneSelectionPickResult Miss() => new(SceneSelectionPickStatus.Miss);
}

public interface ISceneViewportSelectionPicker
{
    SceneSelectionPickResult TryPick(float pointerX, float pointerY, RemoteViewportInputModifier modifiers);
}

public sealed class NullSceneViewportSelectionPicker : ISceneViewportSelectionPicker
{
    public static NullSceneViewportSelectionPicker Instance { get; } = new();

    NullSceneViewportSelectionPicker()
    {
    }

    public SceneSelectionPickResult TryPick(float pointerX, float pointerY, RemoteViewportInputModifier modifiers) => SceneSelectionPickResult.Unavailable();
}

public readonly record struct SceneSelectionRoutingResult(bool Handled, bool ClearSelection = false, string? SelectedId = null)
{
    public static SceneSelectionRoutingResult None { get; } = new(false);
    public static SceneSelectionRoutingResult Clear() => new(true, ClearSelection: true);
    public static SceneSelectionRoutingResult Select(string selectedId) => new(true, SelectedId: selectedId);
}

public sealed class SceneViewportSelectionRouter(ISceneViewportSelectionPicker picker, float clickSlop = 4.0f)
{
    readonly ISceneViewportSelectionPicker _picker = picker ?? throw new ArgumentNullException(nameof(picker));
    readonly float _clickSlopSquared = MathF.Max(0, clickSlop) * MathF.Max(0, clickSlop);

    bool _isLeftPointerDown;
    bool _selectionCandidateArmed;
    float _pressedX;
    float _pressedY;

    public SceneSelectionRoutingResult HandleInput(NativeSceneViewportInputEvent input, bool viewportOwnsSelection)
    {
        if (!viewportOwnsSelection)
        {
            ResetPointerState();
            return SceneSelectionRoutingResult.None;
        }

        switch (input.Kind)
        {
            case Controls.NativeSceneViewportInputKind.PointerButton:
                return HandlePointerButton(input);

            case Controls.NativeSceneViewportInputKind.PointerMove:
                HandlePointerMove(input);
                return SceneSelectionRoutingResult.None;

            case Controls.NativeSceneViewportInputKind.Capture:
                if (!input.Captured)
                {
                    ResetPointerState();
                }
                return SceneSelectionRoutingResult.None;

            case Controls.NativeSceneViewportInputKind.Focus:
                if (!input.Focused)
                {
                    ResetPointerState();
                }
                return SceneSelectionRoutingResult.None;

            default:
                return SceneSelectionRoutingResult.None;
        }
    }

    SceneSelectionRoutingResult HandlePointerButton(NativeSceneViewportInputEvent input)
    {
        if (input.Button != 0)
        {
            if (input.Pressed)
            {
                _selectionCandidateArmed = false;
            }
            return SceneSelectionRoutingResult.None;
        }

        if (input.Pressed)
        {
            _isLeftPointerDown = true;
            _selectionCandidateArmed = !HasDisqualifyingModifiers((RemoteViewportInputModifier)input.Modifiers);
            _pressedX = input.PointerX;
            _pressedY = input.PointerY;
            return SceneSelectionRoutingResult.None;
        }

        var shouldPick = _isLeftPointerDown && _selectionCandidateArmed;
        ResetPointerState();
        if (!shouldPick)
        {
            return SceneSelectionRoutingResult.None;
        }

        var pick = _picker.TryPick(input.PointerX, input.PointerY, (RemoteViewportInputModifier)input.Modifiers);
        return pick.Status switch
        {
            SceneSelectionPickStatus.Hit when !string.IsNullOrWhiteSpace(pick.SelectedId) => SceneSelectionRoutingResult.Select(pick.SelectedId),
            SceneSelectionPickStatus.Miss => SceneSelectionRoutingResult.Clear(),
            _ => SceneSelectionRoutingResult.None,
        };
    }

    void HandlePointerMove(NativeSceneViewportInputEvent input)
    {
        if (!_isLeftPointerDown || !_selectionCandidateArmed)
        {
            return;
        }

        var deltaX = input.PointerX - _pressedX;
        var deltaY = input.PointerY - _pressedY;
        if ((deltaX * deltaX) + (deltaY * deltaY) > _clickSlopSquared)
        {
            _selectionCandidateArmed = false;
        }
    }

    static bool HasDisqualifyingModifiers(RemoteViewportInputModifier modifiers)
    {
        const RemoteViewportInputModifier disallowed = RemoteViewportInputModifier.Shift |
                                                       RemoteViewportInputModifier.Control |
                                                       RemoteViewportInputModifier.Alt |
                                                       RemoteViewportInputModifier.Meta |
                                                       RemoteViewportInputModifier.MouseRight |
                                                       RemoteViewportInputModifier.MouseMiddle;
        return (modifiers & disallowed) != 0;
    }

    void ResetPointerState()
    {
        _isLeftPointerDown = false;
        _selectionCandidateArmed = false;
        _pressedX = 0;
        _pressedY = 0;
    }
}
