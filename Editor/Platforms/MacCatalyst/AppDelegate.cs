using Foundation;
using ObjCRuntime;
using SailorEditor.Services;
using UIKit;

namespace SailorEditor;

[Register("AppDelegate")]
public class AppDelegate : MauiUIApplicationDelegate
{
    static readonly Selector NewWorkspaceSelector = new("newWorkspace:");
    static readonly Selector OpenWorkspaceSelector = new("openWorkspace:");
    static readonly Selector SaveWorkspaceSelector = new("saveWorkspace:");

    protected override MauiApp CreateMauiApp() => MauiProgram.CreateMauiApp();

    public override void BuildMenu(IUIMenuBuilder builder)
    {
        base.BuildMenu(builder);

        var workspaceMenu = UIMenu.Create(
            string.Empty,
            null,
            UIMenuIdentifier.File,
            UIMenuOptions.DisplayInline,
            new UIMenuElement[]
            {
                UICommand.Create("New Workspace...", null, NewWorkspaceSelector, null),
                UICommand.Create("Open Workspace...", null, OpenWorkspaceSelector, null),
                UICommand.Create("Save Workspace", null, SaveWorkspaceSelector, null)
            });

        builder.InsertChildMenuAtStart(workspaceMenu, UIMenuIdentifier.File.GetConstant().ToString());
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

    static void RunWorkspaceAction(Func<WorkspaceUiService, Task> action)
    {
        _ = MainThread.InvokeOnMainThreadAsync(async () =>
        {
            await action(MauiProgram.GetService<WorkspaceUiService>());
        });
    }
}
