using SailorEditor.Controls;
using SailorEditor.Scene;
using SailorEditor.Services;

namespace Editor.Tests;

public sealed class SceneSelectionRoutingTests
{
    [Fact]
    public void LeftClickHit_SelectsPickedInstance()
    {
        var picker = new FakePicker(SceneSelectionPickResult.Hit("go-42"));
        var sut = new SceneViewportSelectionRouter(picker);

        sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: true), viewportOwnsSelection: true);
        var result = sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: false), viewportOwnsSelection: true);

        Assert.True(result.Handled);
        Assert.False(result.ClearSelection);
        Assert.Equal("go-42", result.SelectedId);
        Assert.Equal(1, picker.CallCount);
    }

    [Fact]
    public void LeftClickMiss_ClearsSelection()
    {
        var sut = new SceneViewportSelectionRouter(new FakePicker(SceneSelectionPickResult.Miss()));

        sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: true), viewportOwnsSelection: true);
        var result = sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: false), viewportOwnsSelection: true);

        Assert.True(result.Handled);
        Assert.True(result.ClearSelection);
        Assert.Null(result.SelectedId);
    }

    [Fact]
    public void UnavailablePicker_DoesNotTouchSelection()
    {
        var sut = new SceneViewportSelectionRouter(new FakePicker(SceneSelectionPickResult.Unavailable()));

        sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: true), viewportOwnsSelection: true);
        var result = sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: false), viewportOwnsSelection: true);

        Assert.False(result.Handled);
        Assert.False(result.ClearSelection);
        Assert.Null(result.SelectedId);
    }

    [Fact]
    public void DragPastSlop_DoesNotAttemptPick()
    {
        var picker = new FakePicker(SceneSelectionPickResult.Hit("go-42"));
        var sut = new SceneViewportSelectionRouter(picker, clickSlop: 4);

        sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: true), viewportOwnsSelection: true);
        sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerMove, 20, 20), viewportOwnsSelection: true);
        var result = sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 20, 20, Button: 0, Pressed: false), viewportOwnsSelection: true);

        Assert.False(result.Handled);
        Assert.Equal(0, picker.CallCount);
    }

    [Fact]
    public void SelectionDoesNotRouteWithoutViewportOwnership()
    {
        var picker = new FakePicker(SceneSelectionPickResult.Hit("go-42"));
        var sut = new SceneViewportSelectionRouter(picker);

        sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: true), viewportOwnsSelection: false);
        var result = sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Pressed: false), viewportOwnsSelection: false);

        Assert.False(result.Handled);
        Assert.Equal(0, picker.CallCount);
    }

    [Fact]
    public void ModifierClick_DoesNotAttemptPick()
    {
        var picker = new FakePicker(SceneSelectionPickResult.Hit("go-42"));
        var sut = new SceneViewportSelectionRouter(picker);

        sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Modifiers: NativeSceneViewportInputModifier.Shift, Pressed: true), viewportOwnsSelection: true);
        var result = sut.HandleInput(new NativeSceneViewportInputEvent(NativeSceneViewportInputKind.PointerButton, 10, 20, Button: 0, Modifiers: NativeSceneViewportInputModifier.Shift, Pressed: false), viewportOwnsSelection: true);

        Assert.False(result.Handled);
        Assert.Equal(0, picker.CallCount);
    }

    sealed class FakePicker(SceneSelectionPickResult result) : ISceneViewportSelectionPicker
    {
        public int CallCount { get; private set; }

        public SceneSelectionPickResult TryPick(float pointerX, float pointerY, RemoteViewportInputModifier modifiers)
        {
            CallCount++;
            return result;
        }
    }
}
