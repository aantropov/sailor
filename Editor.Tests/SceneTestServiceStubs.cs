namespace SailorEditor.Services;

public enum RemoteViewportInputKind : uint
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
public enum RemoteViewportInputModifier : uint
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

public enum RemoteViewportSessionState : uint
{
    Created = 0,
    Negotiating = 1,
    Ready = 2,
    Active = 3,
    Paused = 4,
    Resizing = 5,
    Recovering = 6,
    Lost = 7,
    Terminating = 8,
    Disposed = 9
}
