using Microsoft.Maui.Hosting;
using Microsoft.Maui.Platforms.Linux.Gtk4.Platform;

namespace SailorEditor;

public sealed class Program : GtkMauiApplication
{
    protected override MauiApp CreateMauiApp()
    {
        LinuxMainThread.Initialize();
        return MauiProgram.CreateMauiApp();
    }

    public static void Main(string[] args)
    {
        // GTK's own GPU renderer can compete with the engine's Vulkan device.
        // Keep widget composition on Cairo; the scene still renders with Vulkan.
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("GSK_RENDERER")))
        {
            Environment.SetEnvironmentVariable("GSK_RENDERER", "cairo");
        }
        var application = new Program();
        application.Run(args);
    }
}
