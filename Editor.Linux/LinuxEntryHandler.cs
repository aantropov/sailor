using Microsoft.Maui;
using Microsoft.Maui.Platforms.Linux.Gtk4.Handlers;

namespace SailorEditor.Platforms.Linux;

internal sealed class LinuxEntryHandler : EntryHandler
{
    internal static void Configure()
    {
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
}
