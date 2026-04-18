using SailorEditor.Views;
using Microsoft.Extensions.Logging;
using SailorEditor.Helpers;
using SailorEditor.Services;
using CommunityToolkit.Maui;
using SailorEditor.Controls;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.State;
#if MACCATALYST
using SailorEditor.Platforms.MacCatalyst;
#endif

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
#if MACCATALYST
                .ConfigureMauiHandlers(handlers =>
                {
                    handlers.AddHandler<NativeSceneViewport, NativeSceneViewportHandler>();
                })
#endif
                .ConfigureFonts(fonts =>
                {
                    fonts.AddFont("OpenSans-Regular.ttf", "OpenSansRegular");
                    fonts.AddFont("OpenSans-Semibold.ttf", "OpenSansSemibold");
                });

            builder.Services.AddSingleton<AssetsService>();
            builder.Services.AddSingleton<SelectionService>();
            builder.Services.AddSingleton<WorldService>();
            builder.Services.AddSingleton<EngineService>();
            builder.Services.AddSingleton<EditorContextMenuService>();
            builder.Services.AddSingleton<PanelRegistry>();
            builder.Services.AddSingleton<ShellState>();
            builder.Services.AddSingleton<IEditorShellLayoutStore, YamlEditorShellLayoutStore>();
            builder.Services.AddSingleton<EditorShellHost>();
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
