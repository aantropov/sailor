namespace SailorEditor.Utility;

public readonly record struct EditorInputModifierState(bool Shift, bool Additive);

public static class EditorInputModifiers
{
    public static EditorInputModifierState Current
    {
        get
        {
#if MACCATALYST || IOS
            var keyboard = GameController.GCKeyboard.CoalescedKeyboard?.KeyboardInput;
            bool Pressed(nint key) => keyboard?.GetButton(key)?.IsPressed == true;
            return new(
                Pressed(GameController.GCKeyCode.LeftShift) || Pressed(GameController.GCKeyCode.RightShift),
                Pressed(GameController.GCKeyCode.LeftControl) || Pressed(GameController.GCKeyCode.RightControl) ||
                Pressed(GameController.GCKeyCode.LeftGui) || Pressed(GameController.GCKeyCode.RightGui));
#elif WINDOWS
            bool Pressed(Windows.System.VirtualKey key) =>
                (Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(key) &
                    Windows.UI.Core.CoreVirtualKeyStates.Down) != 0;
            return new(Pressed(Windows.System.VirtualKey.Shift), Pressed(Windows.System.VirtualKey.Control));
#else
            return default;
#endif
        }
    }
}
