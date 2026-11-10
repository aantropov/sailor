using System.Globalization;
using Microsoft.Maui;
using Microsoft.Maui.Platforms.Linux.Gtk4.Handlers;

namespace SailorEditor.Platforms.Linux;

internal sealed class LinuxBorderHandler : BorderHandler
{
    internal static void Configure()
    {
        foreach (var property in new[]
        {
            nameof(IBorderView.Stroke), nameof(IBorderView.StrokeThickness),
            nameof(IBorderView.Background), nameof(IBorderView.Shape)
        })
        {
            Mapper.AppendToMapping(property, (handler, border) =>
            {
                if (handler is LinuxBorderHandler linux)
                {
                    linux.ApplyStrokeWidth(border);
                }
            });
        }
    }

    void ApplyStrokeWidth(IBorderView border)
    {
        // The pinned GTK handler clamps every solid stroke to at least 1px,
        // including unselected rows whose MAUI StrokeThickness is zero.
        var width = Math.Max(0, border.StrokeThickness).ToString("0.###", CultureInfo.InvariantCulture);
        PlatformView.AddCssClass("sailor-border");
        UpdateCssWithSelector(PlatformView, ".sailor-border", $"border-width: {width}px;", "SailorStrokeWidth");
    }
}
