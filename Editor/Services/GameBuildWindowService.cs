#nullable enable

using SailorEditor.Views;
using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class GameBuildWindowService(EngineService engine, GameBuildService builds)
{
    Window? _window;

    public Task ShowAsync() => MainThread.InvokeOnMainThreadAsync(async () =>
    {
        if (_window is not null)
        {
            return;
        }
        try
        {
            var context = engine.GetLaunchContext();
            var page = new GameBuildPage(context.WorkspaceRoot, engine.EngineWorkingDirectory, builds);
            var window = new Window(page) { Title = "Build Game", Width = 780, Height = 900,
                MinimumWidth = 620, MinimumHeight = 600 };
            window.Destroying += (_, _) =>
            {
                page.Cancel();
                _window = null;
            };
            _window = window;
            Application.Current!.OpenWindow(window);
        }
        catch (Exception error)
        {
            _window = null;
            var page = Application.Current?.Windows.FirstOrDefault()?.Page;
            if (page is not null)
            {
                await page.DisplayAlert("Build Game", error.Message, "OK");
            }
        }
    });
}
