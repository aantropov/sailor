using Foundation;
using ObjCRuntime;
using SailorEditor.Services;
using SailorEditor.Workspace;
using UIKit;

namespace SailorEditor;

[Register("AppDelegate")]
public class AppDelegate : MauiUIApplicationDelegate
{
    static readonly Selector NewWorkspaceSelector = new("newWorkspace:");
    static readonly Selector OpenWorkspaceSelector = new("openWorkspace:");
    static readonly Selector SaveWorkspaceSelector = new("saveWorkspace:");
    static readonly Selector OpenRecentWorkspaceSelector = new("openRecentWorkspace:");
    static readonly Selector NewSceneSelector = new("newScene:");
    static readonly Selector ReloadAssetsSelector = new("reloadAssets:");
    static IReadOnlyList<WorkspaceRecentItem> recentWorkspaces = [];

    protected override MauiApp CreateMauiApp() => MauiProgram.CreateMauiApp();

    public override void WillTerminate(UIApplication application)
    {
        try
        {
            var shutdownTask =
                MauiProgram.GetService<EngineService>()
                    .DisposeAsync()
                    .AsTask();
            while (!shutdownTask.IsCompleted)
            {
                using var deadline =
                    NSDate.FromTimeIntervalSinceNow(0.01);
                NSRunLoop.Main.RunUntil(deadline);
            }

            shutdownTask.GetAwaiter().GetResult();
        }
        catch (Exception exception)
        {
            Console.WriteLine(
                $"[EngineService] Application termination drain failed: {exception.Message}");
        }
        finally
        {
            base.WillTerminate(application);
        }
    }

    public override void BuildMenu(IUIMenuBuilder builder)
    {
        base.BuildMenu(builder);

        var workspaceMenu = UIMenu.Create(
            string.Empty,
            null,
            new NSString("com.sailor.workspace"),
            UIMenuOptions.DisplayInline,
            new UIMenuElement[]
            {
                UICommand.Create("New Workspace...", null, NewWorkspaceSelector, null),
                UICommand.Create("Open Workspace...", null, OpenWorkspaceSelector, null),
                UICommand.Create("Save Workspace", null, SaveWorkspaceSelector, null),
                BuildRecentWorkspacesMenu(),
                UICommand.Create("New Scene", null, NewSceneSelector, null),
                UIKeyCommand.Create(
                    "Reload Assets",
                    null,
                    ReloadAssetsSelector,
                    "r",
                    UIKeyModifierFlags.Command,
                    null)
            });

        builder.InsertChildMenuAtStart(workspaceMenu, UIMenuIdentifier.File.GetConstant().ToString());
    }

    public static void RequestMenuRebuild()
    {
        if (!MainThread.IsMainThread)
        {
            MainThread.BeginInvokeOnMainThread(RequestMenuRebuild);
            return;
        }

        UIMenuSystem.MainSystem.SetNeedsRebuild();
    }

    public static void UpdateRecentWorkspaces(IReadOnlyList<WorkspaceRecentItem> recent)
    {
        var snapshot = recent?.ToArray() ?? [];
        if (!MainThread.IsMainThread)
        {
            MainThread.BeginInvokeOnMainThread(() => UpdateRecentWorkspaces(snapshot));
            return;
        }

        recentWorkspaces = snapshot;
        RequestMenuRebuild();
    }

    [Export("newWorkspace:")]
    public void NewWorkspace(NSObject sender)
        => RunWorkspaceAction(workspace => workspace.NewWorkspaceAsync());

    [Export("openWorkspace:")]
    public void OpenWorkspace(NSObject sender)
        => RunWorkspaceAction(workspace => workspace.OpenWorkspaceAsync());

    [Export("saveWorkspace:")]
    public void SaveWorkspace(NSObject sender)
        => RunWorkspaceAction(workspace => workspace.SaveWorkspaceAsync());

    [Export("openRecentWorkspace:")]
    public void OpenRecentWorkspace(NSObject sender)
    {
        if (sender is not UICommand command || command.PropertyList is not NSString manifestPath || string.IsNullOrWhiteSpace(manifestPath.ToString()))
            return;

        RunWorkspaceAction(workspace => workspace.OpenWorkspaceAsync(manifestPath.ToString()));
    }

    [Export("newScene:")]
    public void NewScene(NSObject sender)
        => _ = MainThread.InvokeOnMainThreadAsync(
            () => MauiProgram.GetService<EditorToolbarActions>().NewSceneAsync());

    [Export("reloadAssets:")]
    public void ReloadAssets(NSObject sender)
        => _ = MauiProgram.GetService<EngineService>().RequestAssetReloadAsync();

    static UIMenu BuildRecentWorkspacesMenu()
    {
        var recent = recentWorkspaces;
        if (recent.Count == 0)
        {
            return UIMenu.Create(
                "Recent Workspaces",
                null,
                new NSString("com.sailor.workspace.recent"),
                0,
                new UIMenuElement[]
                {
                    UICommand.Create("No recent workspaces", null, OpenRecentWorkspaceSelector, null)
                });
        }

        var commands = recent
            .Select(workspace => UICommand.Create(
                $"{workspace.Name} - {workspace.DisplayPath}",
                null,
                OpenRecentWorkspaceSelector,
                new NSString(workspace.ManifestPath)))
            .Cast<UIMenuElement>()
            .ToArray();

        return UIMenu.Create(
            "Recent Workspaces",
            null,
            new NSString("com.sailor.workspace.recent"),
            0,
            commands);
    }

    static void RunWorkspaceAction(Func<WorkspaceUiService, Task> action)
    {
        _ = MainThread.InvokeOnMainThreadAsync(async () =>
        {
            await action(MauiProgram.GetService<WorkspaceUiService>());
        });
    }
}
