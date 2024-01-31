using Editor.Views;
using Microsoft.Extensions.Logging;
using Microsoft.Maui.Controls.Compatibility.Hosting;
using Microsoft.Maui.Embedding;
using Editor.Helpers;
using Editor.Services;

namespace Editor
{
    public static class MauiProgram
    {
        static IServiceProvider serviceProvider;
        public static TService GetService<TService>() => serviceProvider.GetService<TService>();
        public static MauiApp CreateMauiApp()
        {
            var builder = MauiApp.CreateBuilder();
            builder
                .UseMauiApp<App>()
                //.UseMauiCompatibility()
                .ConfigureFonts(fonts =>
                {
                    fonts.AddFont("OpenSans-Regular.ttf", "OpenSansRegular");
                    fonts.AddFont("OpenSans-Semibold.ttf", "OpenSansSemibold");
                });

            builder.Services.AddSingleton<AssetsService>();
            builder.Services.AddSingleton<SelectionService>();
            builder.Services.AddSingleton<FolderTreeViewBuilder>();
            builder.Services.AddTransient<MainPage>();
            builder.Services.AddTransient<ContentFolderView>();

#if DEBUG
            builder.Logging.AddDebug();
#endif

            // We need that to pass DI into Views
            var app = builder.Build();
            serviceProvider = app.Services;
            return app;
        }
    }
}