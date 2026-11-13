#nullable enable

using SailorEditor.Views;
using SailorEditor.Workspace;

namespace SailorEditor.Services;

internal sealed class GameBuildWindowService(EngineService engine, GameBuildService builds,
    WorkspaceLifecycleService workspaces, WorldService worlds)
{
    GameBuildPage? _page;

    public Task ShowAsync() => MainThread.InvokeOnMainThreadAsync(async () =>
    {
        if (_page is not null)
        {
            return;
        }
        try
        {
            var context = engine.GetLaunchContext();
            var workspace = workspaces.Current;
            var engineDirectory = workspace is null ? engine.EngineWorkingDirectory :
                Path.GetFullPath(workspace.Manifest.EnginePath, workspace.WorkspaceRoot);
            var currentScene = worlds.CurrentWorldAsset?.Asset?.FullName;
            var page = new GameBuildPage(context.WorkspaceRoot, engineDirectory, builds, context.ContentDirectory,
                currentScene is null ? null : Path.GetRelativePath(context.ContentDirectory, currentScene).Replace('\\', '/'));
            _page = page;
#if MACCATALYST
            // Keep export controls in the editor's window scope on Catalyst.
            var owner = Application.Current!.Windows.First().Page!;
            var navigation = new NavigationPage(page);
            Microsoft.Maui.Controls.PlatformConfiguration.iOSSpecific.Page.SetModalPresentationStyle(
                navigation.On<Microsoft.Maui.Controls.PlatformConfiguration.iOS>(),
                Microsoft.Maui.Controls.PlatformConfiguration.iOSSpecific.UIModalPresentationStyle.FormSheet);
            page.ToolbarItems.Add(new ToolbarItem
            {
                Text = "Close",
                Command = new Command(async () =>
                {
                    await page.CancelAsync();
                    await owner.Navigation.PopModalAsync();
                    _page = null;
                })
            });
            page.Disappearing += (_, _) => MainThread.BeginInvokeOnMainThread(() =>
            {
                if (!owner.Navigation.ModalStack.Contains(navigation))
                {
                    page.Cancel();
                    _page = null;
                }
            });
            await owner.Navigation.PushModalAsync(navigation);
#else
            var window = new Window(page) { Title = "Build Game", Width = 900, Height = 780,
                MinimumWidth = 700, MinimumHeight = 650 };
            window.Destroying += (_, _) =>
            {
                page.Cancel();
                _page = null;
            };
            Application.Current!.OpenWindow(window);
#endif
        }
        catch (Exception error)
        {
            _page = null;
            var page = Application.Current?.Windows.FirstOrDefault()?.Page;
            if (page is not null)
            {
                await page.DisplayAlert("Build Game", error.Message, "OK");
            }
        }
    });
}
