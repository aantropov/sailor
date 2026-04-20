using SailorEditor.Views;
using Microsoft.Extensions.Logging;
using SailorEditor.Helpers;
using SailorEditor.Services;
using CommunityToolkit.Maui;
using SailorEditor.Controls;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.State;
using SailorEditor.Commands;
using SailorEditor.History;
using SailorEditor.Content;
using SailorEditor.Settings;
using SailorEditor.AI;
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
            builder.Services.AddSingleton<ProjectContentStore>();
            builder.Services.AddSingleton<SelectionService>();
            builder.Services.AddSingleton<WorldService>();
            builder.Services.AddSingleton<HierarchyProjectionService>();
            builder.Services.AddSingleton<InspectorProjectionService>();
            builder.Services.AddSingleton<EngineService>();
            builder.Services.AddSingleton<EditorContextMenuService>();
            builder.Services.AddSingleton<PanelRegistry>();
            builder.Services.AddSingleton<ShellState>();
            builder.Services.AddSingleton<IUndoRedoHistory, InMemoryUndoRedoHistory>();
            builder.Services.AddSingleton<IActionContextProvider, EditorActionContextProvider>();
            builder.Services.AddSingleton<EditorCommandDispatcher>();
            builder.Services.AddSingleton<ICommandDispatcher>(sp => sp.GetRequiredService<EditorCommandDispatcher>());
            builder.Services.AddSingleton<ICommandHistoryService>(sp => sp.GetRequiredService<EditorCommandDispatcher>());
            builder.Services.AddSingleton<ITransactionScopeFactory>(sp => sp.GetRequiredService<EditorCommandDispatcher>());
            builder.Services.AddSingleton<IEditorShellLayoutStore, YamlEditorShellLayoutStore>();
            builder.Services.AddSingleton(sp => new UnifiedSettingsStore(EditorSettingsCatalog.Definitions));
            builder.Services.AddSingleton<EditorSettingsPersistenceStore>();
            builder.Services.AddSingleton<EditorShellHost>();
            builder.Services.AddSingleton<IAIAgentInstructionsProvider, MarkdownAIAgentInstructionsProvider>();
            builder.Services.AddSingleton<IAIEditorContextProvider, EditorAIContextProvider>();
            builder.Services.AddSingleton<IAIActionPlanner, HeuristicAIActionPlanner>();
            builder.Services.AddSingleton<AIOperatorService>();
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
