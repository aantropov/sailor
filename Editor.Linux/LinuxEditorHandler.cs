using Microsoft.Maui;
using Microsoft.Maui.Platforms.Linux.Gtk4.Handlers;

namespace SailorEditor.Platforms.Linux;

internal sealed class LinuxEditorHandler : EditorHandler
{
    bool updatingText;

    internal static void Configure()
    {
        Mapper.ModifyMapping(nameof(IEditor.Text), (handler, editor, original) =>
        {
            if (handler is not LinuxEditorHandler linux)
            {
                original?.Invoke(handler, editor);
                return;
            }
            linux.updatingText = true;
            try
            {
                original?.Invoke(handler, editor);
            }
            finally
            {
                linux.updatingText = false;
            }
        });
    }

    protected override void ConnectHandler(Gtk.TextView platformView)
    {
        platformView.GetBuffer().OnChanged += FilterProgrammaticTextChange;
        base.ConnectHandler(platformView);
    }

    protected override void DisconnectHandler(Gtk.TextView platformView)
    {
        base.DisconnectHandler(platformView);
        platformView.GetBuffer().OnChanged -= FilterProgrammaticTextChange;
    }

    void FilterProgrammaticTextChange(Gtk.TextBuffer sender, EventArgs args)
    {
        // SetText clears the buffer before inserting its replacement. Feeding
        // that intermediate empty value back to MAUI queues an endless update.
        if (updatingText && sender.GetCharCount() == 0 && !string.IsNullOrEmpty(VirtualView.Text))
        {
            GObject.Functions.SignalStopEmissionByName(sender, "changed");
        }
    }
}
