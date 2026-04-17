#nullable enable
namespace SailorEditor.Controls;

public enum NativeSceneViewportInputKind : uint
{
    Unknown = 0,
    PointerMove = 1,
    PointerButton = 2,
    PointerWheel = 3,
    Key = 4,
    Focus = 5,
    Capture = 6
}

[Flags]
public enum NativeSceneViewportInputModifier : uint
{
    None = 0,
    Shift = 1 << 0,
    Control = 1 << 1,
    Alt = 1 << 2,
    Meta = 1 << 3,
    MouseLeft = 1 << 4,
    MouseRight = 1 << 5,
    MouseMiddle = 1 << 6
}

public readonly record struct NativeSceneViewportInputEvent(
    NativeSceneViewportInputKind Kind,
    float PointerX = 0,
    float PointerY = 0,
    float WheelDeltaX = 0,
    float WheelDeltaY = 0,
    uint KeyCode = 0,
    uint Button = 0,
    NativeSceneViewportInputModifier Modifiers = NativeSceneViewportInputModifier.None,
    bool Pressed = false,
    bool Focused = false,
    bool Captured = false);

public interface INativeSceneViewportLayoutHost
{
    void RequestLayoutUpdate(double width, double height, double contentsScale);
}

public class NativeSceneViewport : View
{
    public event Action<nint>? HostHandleChanged;
    public event Action<double, double, double>? HostLayoutChanged;
    public event Action<NativeSceneViewportInputEvent>? InputReceived;

    internal void UpdateHostHandle(nint handle)
    {
        HostHandleChanged?.Invoke(handle);
    }

    internal void UpdateHostLayout(double width, double height, double contentsScale)
    {
        HostLayoutChanged?.Invoke(width, height, contentsScale);
    }

    internal void PublishInput(NativeSceneViewportInputEvent input)
    {
        InputReceived?.Invoke(input);
    }

    public void RequestLayoutUpdate(double width, double height, double contentsScale)
    {
        if (Handler is INativeSceneViewportLayoutHost layoutHost)
        {
            layoutHost.RequestLayoutUpdate(width, height, contentsScale);
        }
    }
}
