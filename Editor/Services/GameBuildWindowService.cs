#nullable enable

using SailorEditor.Views;
using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class GameBuildWindowService(EngineService engine, GameBuildService builds,
    WorkspaceLifecycleService workspaces)
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
            var workspace = workspaces.Current;
            var engineDirectory = workspace is null ? engine.EngineWorkingDirectory :
                Path.GetFullPath(workspace.Manifest.EnginePath, workspace.WorkspaceRoot);
            var page = new GameBuildPage(context.WorkspaceRoot, engineDirectory, builds);
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
