using Microsoft.Maui;
using Microsoft.Maui.Dispatching;
using Microsoft.Maui.Platforms.Linux.Gtk4.Handlers;
using SailorEditor.Controls;
using SailorEditor.Protocol;
using SailorEditor.Services;

namespace SailorEditor.Platforms.Linux;

public sealed class NativeSceneViewportHandler : GtkViewHandler<NativeSceneViewport, Gtk.Picture>, INativeSceneViewportLayoutHost
{
    public static readonly IPropertyMapper<NativeSceneViewport, NativeSceneViewportHandler> Mapper =
        new PropertyMapper<NativeSceneViewport, NativeSceneViewportHandler>(ViewMapper);

    readonly List<Gtk.EventController> controllers = [];
    IDispatcherTimer? timer;
    byte[] pixels = [];
    ulong frameIndex;
    float pointerX;
    float pointerY;
    NativeSceneViewportInputModifier pointerButtons;
    bool focused;
    bool reportedFirstFrame;
    (int Width, int Height, int Scale) lastLayout;

    public NativeSceneViewportHandler() : base(Mapper) { }

    protected override Gtk.Picture CreatePlatformView()
    {
        var picture = Gtk.Picture.New();
        picture.SetCanShrink(true);
        picture.SetContentFit(Gtk.ContentFit.Fill);
        picture.SetFocusable(true);
        return picture;
    }

    protected override void ConnectHandler(Gtk.Picture picture)
    {
        base.ConnectHandler(picture);
        var motion = Gtk.EventControllerMotion.New();
        motion.OnMotion += (_, args) =>
        {
            UpdatePointer(args.X, args.Y);
            Publish(NativeSceneViewportInputKind.PointerMove, motion.GetCurrentEventState());
        };
        AddController(motion);

        var click = Gtk.GestureClick.New();
        click.SetButton(0);
        click.OnPressed += (_, args) =>
        {
            RequestInputFocus();
            UpdatePointer(args.X, args.Y);
            PublishButton(click, true);
        };
        click.OnReleased += (_, args) =>
        {
            UpdatePointer(args.X, args.Y);
            PublishButton(click, false);
        };
        AddController(click);

        var scroll = Gtk.EventControllerScroll.New(Gtk.EventControllerScrollFlags.BothAxes);
        scroll.OnScroll += (_, args) =>
        {
            VirtualView.PublishInput(new(NativeSceneViewportInputKind.PointerWheel,
                pointerX, pointerY, (float)-args.Dx, (float)-args.Dy,
                Modifiers: Modifiers(scroll.GetCurrentEventState())));
            return true;
        };
        AddController(scroll);

        var keys = Gtk.EventControllerKey.New();
        keys.OnKeyPressed += (_, args) =>
        {
            PublishKey(args.Keyval, args.State, true);
            return true;
        };
        keys.OnKeyReleased += (_, args) => PublishKey(args.Keyval, args.State, false);
        AddController(keys);

        var focus = Gtk.EventControllerFocus.New();
        focus.OnLeave += (_, _) => ReleaseInputFocus();
        AddController(focus);
        timer = VirtualView.Dispatcher.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(33);
        timer.Tick += OnTick;
        timer.Start();
        // The portable readback host has no external native surface to retain.
        VirtualView.UpdateHostHandle(picture.Handle.DangerousGetHandle());
    }

    void AddController(Gtk.EventController controller)
    {
        controllers.Add(controller);
        PlatformView.AddController(controller);
    }

    protected override void DisconnectHandler(Gtk.Picture picture)
    {
        if (timer is not null)
        {
            timer.Stop();
            timer.Tick -= OnTick;
            timer = null;
        }
        ReleaseInputFocus();
        foreach (var controller in controllers)
        {
            picture.RemoveController(controller);
            controller.Dispose();
        }
        controllers.Clear();
        picture.SetPaintable(null);
        VirtualView.UpdateHostHandle(nint.Zero);
        pixels = [];
        frameIndex = 0;
        lastLayout = default;
        base.DisconnectHandler(picture);
    }

    void OnTick(object? sender, EventArgs args)
    {
        var engine = MauiProgram.GetService<EngineService>();
        if (!engine.IsRunning)
        {
            frameIndex = 0;
            reportedFirstFrame = false;
            return;
        }
        EngineProtocolNative.SailorProtocolPollLinuxWindowEvents();
        RequestLayoutUpdate(VirtualView.Width, VirtualView.Height, 1);
        var result = EngineProtocolNative.SailorProtocolCopyLinuxViewportFrame(
            pixels, (ulong)pixels.Length, out var width, out var height, out var stride, ref frameIndex);
        if (result == 2)
        {
            pixels = new byte[checked((int)((ulong)stride * height))];
            result = EngineProtocolNative.SailorProtocolCopyLinuxViewportFrame(
                pixels, (ulong)pixels.Length, out width, out height, out stride, ref frameIndex);
        }
        if (result != 1)
        {
            return;
        }
        using var bytes = GLib.Bytes.New(pixels.AsSpan(0, checked((int)((ulong)stride * height))));
        using var texture = Gdk.MemoryTexture.New((int)width, (int)height,
            Gdk.MemoryFormat.B8g8r8a8, bytes, (nuint)stride);
        PlatformView.SetPaintable(texture);
        if (!reportedFirstFrame)
        {
            Console.WriteLine($"[LinuxViewport] Presented engine frame {frameIndex}: {width}x{height}, pitch={stride}");
            reportedFirstFrame = true;
        }
    }

    public void RequestLayoutUpdate(double width, double height, double contentsScale)
    {
        if (PlatformView is null)
        {
            return;
        }
        var allocatedWidth = PlatformView.GetWidth();
        var allocatedHeight = PlatformView.GetHeight();
        if (allocatedWidth <= 0 || allocatedHeight <= 0)
        {
            return;
        }
        var scale = Math.Max(1, PlatformView.GetScaleFactor());
        var layout = (allocatedWidth, allocatedHeight, scale);
        if (lastLayout == layout)
        {
            return;
        }
        lastLayout = layout;
        VirtualView.UpdateHostLayout(allocatedWidth, allocatedHeight, scale,
            checked((uint)(allocatedWidth * scale)), checked((uint)(allocatedHeight * scale)));
    }

    public void RequestInputFocus()
    {
        PlatformView?.GrabFocus();
        if (!focused)
        {
            focused = true;
            VirtualView.PublishInput(new(NativeSceneViewportInputKind.Focus, Focused: true, IsPointerFocus: true));
        }
    }

    public void ReleaseInputFocus()
    {
        if (focused)
        {
            focused = false;
            pointerButtons = NativeSceneViewportInputModifier.None;
            VirtualView.PublishInput(new(NativeSceneViewportInputKind.Focus, Focused: false));
        }
    }

    void UpdatePointer(double x, double y)
    {
        var scale = Math.Max(1, PlatformView.GetScaleFactor());
        pointerX = (float)(x * scale);
        pointerY = (float)(y * scale);
    }

    void Publish(NativeSceneViewportInputKind kind, Gdk.ModifierType state) =>
        VirtualView.PublishInput(new(kind, pointerX, pointerY, Modifiers: Modifiers(state)));

    void PublishButton(Gtk.GestureClick gesture, bool pressed)
    {
        var button = gesture.GetCurrentButton() switch { 1 => 0u, 3 => 1u, 2 => 2u, _ => uint.MaxValue };
        if (button > 2)
        {
            return;
        }
        var modifier = button switch
        {
            0 => NativeSceneViewportInputModifier.MouseLeft,
            1 => NativeSceneViewportInputModifier.MouseRight,
            _ => NativeSceneViewportInputModifier.MouseMiddle
        };
        pointerButtons = pressed ? pointerButtons | modifier : pointerButtons & ~modifier;
        VirtualView.PublishInput(new(NativeSceneViewportInputKind.PointerButton,
            pointerX, pointerY, Button: button, Modifiers: Modifiers(gesture.GetCurrentEventState()), Pressed: pressed));
    }

    NativeSceneViewportInputModifier Modifiers(Gdk.ModifierType state)
    {
        var result = pointerButtons;
        if (state.HasFlag(Gdk.ModifierType.ShiftMask))
        {
            result |= NativeSceneViewportInputModifier.Shift;
        }
        if (state.HasFlag(Gdk.ModifierType.ControlMask))
        {
            result |= NativeSceneViewportInputModifier.Control;
        }
        if (state.HasFlag(Gdk.ModifierType.AltMask))
        {
            result |= NativeSceneViewportInputModifier.Alt;
        }
        if (state.HasFlag(Gdk.ModifierType.SuperMask))
        {
            result |= NativeSceneViewportInputModifier.Meta;
        }
        return result;
    }

    void PublishKey(uint key, Gdk.ModifierType state, bool pressed)
    {
        var code = MapKey(key);
        if (code != 0)
        {
            VirtualView.PublishInput(new(NativeSceneViewportInputKind.Key,
                KeyCode: code, Modifiers: Modifiers(state), Pressed: pressed));
        }
        if (pressed && !state.HasFlag(Gdk.ModifierType.ControlMask) && !state.HasFlag(Gdk.ModifierType.AltMask))
        {
            var unicode = Gdk.Functions.KeyvalToUnicode(key);
            if (unicode >= 32 && System.Text.Rune.IsValid(unicode))
            {
                VirtualView.PublishInput(new(NativeSceneViewportInputKind.Text, Text: char.ConvertFromUtf32((int)unicode)));
            }
        }
    }

    static uint MapKey(uint key) => key switch
    {
        >= 'a' and <= 'z' => key - 32,
        >= 'A' and <= 'Z' or >= '0' and <= '9' => key,
        >= 0xffbe and <= 0xffd5 => key - 0xffbe + 0x70,
        0xff08 => 8, 0xff09 => 9, 0xff0d => 13, 0xff1b => 27, 0x20 => 32,
        0xff50 => 0x24, 0xff51 => 0x25, 0xff52 => 0x26, 0xff53 => 0x27, 0xff54 => 0x28,
        0xff55 => 0x21, 0xff56 => 0x22, 0xff57 => 0x23, 0xffff => 0x2e,
        0xffe1 => 0xa0, 0xffe2 => 0xa1, 0xffe3 => 0xa2, 0xffe4 => 0xa3,
        0xffe9 => 0xa4, 0xffea => 0xa5, 0xffeb => 0x5b, 0xffec => 0x5c,
        _ => 0
    };
}
