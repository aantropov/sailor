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
    static readonly Selector ReloadAssetsSelector = new("reloadAssets:");

    protected override MauiApp CreateMauiApp() => MauiProgram.CreateMauiApp();

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

    [Export("reloadAssets:")]
    public void ReloadAssets(NSObject sender)
        => MauiProgram.GetService<EngineService>().RequestAssetReload();

    static UIMenu BuildRecentWorkspacesMenu()
    {
        var recent = LoadRecentWorkspaces();
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

    static IReadOnlyList<WorkspaceRecentItem> LoadRecentWorkspaces()
    {
        try
        {
            var workspaceLifecycle = MauiProgram.GetService<WorkspaceLifecycleService>();
            var recent = MauiProgram.GetService<RecentWorkspaceStore>().Load();
            return WorkspaceUiProjectionBuilder.Build(workspaceLifecycle.Current, recent).RecentWorkspaces;
        }
        catch
        {
            return [];
        }
    }

    static void RunWorkspaceAction(Func<WorkspaceUiService, Task> action)
    {
        _ = MainThread.InvokeOnMainThreadAsync(async () =>
        {
            await action(MauiProgram.GetService<WorkspaceUiService>());
        });
    }
}
