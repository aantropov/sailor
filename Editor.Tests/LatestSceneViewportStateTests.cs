using SailorEditor.Services;

namespace Editor.Tests;

public sealed class LatestSceneViewportStateTests
{
    [Fact]
    public void Capture_ReturnsOneCoherentLatestSceneViewportState()
    {
        var initialRect = new TestRect(0, 0, 1024, 768);
        var firstRect = new TestRect(50, 75, 1280, 720);
        var latestRect = new TestRect(80, 100, 1600, 900);
        var state = new LatestSceneViewportState<TestRect>(initialRect);

        state.Remember(firstRect, visible: true, focused: true);
        var firstSnapshot = state.Capture();
        state.Remember(latestRect, visible: false, focused: false);
        var latestSnapshot = state.Capture();

        Assert.Equal(new SceneViewportStateSnapshot<TestRect>(firstRect, true, true), firstSnapshot);
        Assert.Equal(new SceneViewportStateSnapshot<TestRect>(latestRect, false, false), latestSnapshot);
    }

    [Fact]
    public void RememberRect_PreservesLatestVisibilityAndFocus()
    {
        var state = new LatestSceneViewportState<TestRect>(new TestRect(0, 0, 1024, 768));
        var resizedRect = new TestRect(20, 30, 1920, 1080);

        state.Remember(new TestRect(10, 15, 1280, 720), visible: false, focused: true);
        state.RememberRect(resizedRect);

        Assert.Equal(
            new SceneViewportStateSnapshot<TestRect>(resizedRect, false, true),
            state.Capture());
    }

    readonly record struct TestRect(double X, double Y, double Width, double Height);
}
