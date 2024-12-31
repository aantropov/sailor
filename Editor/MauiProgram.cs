using SailorEditor.Views;
using Microsoft.Extensions.Logging;
using SailorEditor.Helpers;
using SailorEditor.Services;
using CommunityToolkit.Maui;

namespace SailorEditor
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
                .UseMauiCommunityToolkit()
                .ConfigureFonts(fonts =>
                {
                    fonts.AddFont("OpenSans-Regular.ttf", "OpenSansRegular");
                    fonts.AddFont("OpenSans-Semibold.ttf", "OpenSansSemibold");
                });

            builder.Services.AddSingleton<AssetsService>();
            builder.Services.AddSingleton<SelectionService>();
            builder.Services.AddSingleton<WorldService>();
            builder.Services.AddSingleton<EngineService>();
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