using Microsoft.Maui;
using Microsoft.Maui.Platforms.Linux.Gtk4.Handlers;

namespace SailorEditor.Platforms.Linux;

internal sealed class LinuxEntryHandler : EntryHandler
{
    bool updatingText;

    internal static void Configure()
    {
        Mapper.ModifyMapping(nameof(IEntry.Text), (handler, entry, original) =>
        {
            if (handler is not LinuxEntryHandler linux)
            {
                original?.Invoke(handler, entry);
                return;
            }
            linux.updatingText = true;
            try
            {
                original?.Invoke(handler, entry);
            }
            finally
            {
                linux.updatingText = false;
            }
        });
        Mapper.AppendToMapping(nameof(IEntry.ClearButtonVisibility), (handler, entry) =>
        {
            if (handler is LinuxEntryHandler && entry.ClearButtonVisibility == ClearButtonVisibility.Never)
            {
                // The pinned backend leaves an empty clear-button image visible,
                // reserving space even when the clear button is disabled.
                for (var child = handler.PlatformView.GetFirstChild(); child is not null; child = child.GetNextSibling())
                {
                    if (child is Gtk.Image image && image.GetStorageType() == Gtk.ImageType.Empty)
                    {
                        image.SetVisible(false);
                    }
                }
            }
        });
    }

    protected override void ConnectHandler(Gtk.Entry platformView)
    {
        // GTK emits an intermediate empty value while replacing text. Prevent
        // the pinned backend from feeding that value back into MAUI's mapper.
        platformView.OnChanged += FilterProgrammaticTextChange;
        base.ConnectHandler(platformView);
        if (VirtualView.Keyboard == Keyboard.Numeric)
        {
            // GTK's entry/text padding otherwise consumes narrow vector fields.
            platformView.AddCssClass("sailor-numeric");
            platformView.SetWidthChars(1);
            UpdateCssWithSelector(platformView, "entry.sailor-numeric",
                "min-width: 0; padding: 0 4px;", "NumericEntry");
        }
    }

    protected override void DisconnectHandler(Gtk.Entry platformView)
    {
        base.DisconnectHandler(platformView);
        platformView.OnChanged -= FilterProgrammaticTextChange;
    }

    void FilterProgrammaticTextChange(Gtk.Editable sender, EventArgs args)
    {
        if (updatingText && string.IsNullOrEmpty(sender.GetText()) && !string.IsNullOrEmpty(VirtualView.Text))
        {
            GObject.Functions.SignalStopEmissionByName(PlatformView, "changed");
        }
    }
}
