using AppKit;
using CoreFoundation;
using Foundation;
using ObjCRuntime;
using SailorEditor.Services;
using UIKit;
using MauiWindow = Microsoft.Maui.Controls.Window;

namespace SailorEditor.Platforms.MacCatalyst
{
    public static class MacCatalystWindowChrome
    {
        const string SaveItem = "SailorEditorToolbarSave";
        const string UndoItem = "SailorEditorToolbarUndo";
        const string RedoItem = "SailorEditorToolbarRedo";
        const string PlayItem = "SailorEditorToolbarPlay";
        const string DebugItem = "SailorEditorToolbarDebug";
        const string TraceSceneItem = "SailorEditorToolbarTraceScene";
        const string TraceSelectionItem = "SailorEditorToolbarTraceSelection";
        const string SaveLayoutItem = "SailorEditorToolbarSaveLayout";
        const string ResetLayoutItem = "SailorEditorToolbarResetLayout";
        const string SettingsItem = "SailorEditorToolbarSettings";
        static readonly string[] ToolbarItems =
        [
            SaveItem,
            UndoItem,
            RedoItem,
            NSToolbar.NSToolbarFlexibleSpaceItemIdentifier,
            PlayItem,
            DebugItem,
            NSToolbar.NSToolbarFlexibleSpaceItemIdentifier,
            TraceSceneItem,
            TraceSelectionItem,
            SaveLayoutItem,
            ResetLayoutItem,
            SettingsItem
        ];

        static readonly NSSet<NSString> CenterToolbarItems = new(new NSString[]
        {
            new NSString(PlayItem),
            new NSString(DebugItem)
        });

        static readonly NativeToolbarDelegate ToolbarDelegate = new();
        static readonly NSToolbar CompactToolbar = CreateCompactToolbar();
        static NSObject? windowVisibleObserver;

        public static void UseCompactTitlebar(MauiWindow window)
        {
            windowVisibleObserver ??= UIWindow.Notifications.ObserveDidBecomeVisible((_, _) => ApplySoon());

            window.Created += OnWindowChanged;
            window.Activated += OnWindowChanged;
            window.HandlerChanged += OnWindowChanged;

            ApplySoon();
        }

        static void OnWindowChanged(object sender, EventArgs e)
        {
            ApplySoon();
        }

        static void ApplySoon()
        {
            DispatchQueue.MainQueue.DispatchAsync(Apply);
            _ = ApplyLater(100);
            _ = ApplyLater(350);
        }

        static async Task ApplyLater(int delayMs)
        {
            await Task.Delay(delayMs);
            Microsoft.Maui.ApplicationModel.MainThread.BeginInvokeOnMainThread(Apply);
        }

        static void Apply()
        {
            foreach (var window in Application.Current?.Windows ?? [])
            {
                window.Title = string.Empty;
            }

            foreach (var scene in UIApplication.SharedApplication.ConnectedScenes)
            {
                if (scene is not UIWindowScene windowScene || windowScene.Titlebar is not { } titlebar)
                {
                    continue;
                }

                titlebar.TitleVisibility = UITitlebarTitleVisibility.Hidden;
                titlebar.Toolbar = CompactToolbar;

                if (OperatingSystem.IsMacOSVersionAtLeast(12))
                {
                    titlebar.ToolbarStyle = UITitlebarToolbarStyle.UnifiedCompact;
                }
            }

            CollapseTopSafeArea();
        }

        static void CollapseTopSafeArea()
        {
            foreach (var scene in UIApplication.SharedApplication.ConnectedScenes)
            {
                if (scene is not UIWindowScene windowScene)
                {
                    continue;
                }

                foreach (var window in windowScene.Windows)
                {
                    CollapseTopSafeArea(window);
                }
            }
        }

        static void CollapseTopSafeArea(UIWindow window)
        {
            if (window.RootViewController is null)
            {
                return;
            }

            var topInset = window.SafeAreaInsets.Top;
            window.RootViewController.AdditionalSafeAreaInsets = new UIEdgeInsets(-topInset, 0, 0, 0);
        }

        static NSToolbar CreateCompactToolbar()
        {
            var toolbar = new NSToolbar("SailorEditorCompactTitlebar")
            {
                AllowsDisplayModeCustomization = false,
                AllowsUserCustomization = false,
                AutosavesConfiguration = false,
                DisplayMode = NSToolbarDisplayMode.Icon,
                ShowsBaselineSeparator = false,
                SizeMode = NSToolbarSizeMode.Small
            };

            toolbar.Delegate = ToolbarDelegate;
            if (OperatingSystem.IsMacCatalystVersionAtLeast(16))
            {
                toolbar.CenteredItemIdentifiers = CenterToolbarItems;
            }

            return toolbar;
        }

        sealed class NativeToolbarDelegate : NSToolbarDelegate
        {
            static readonly Selector InvokeSelector = new("invoke:");
            readonly Dictionary<string, ToolbarActionTarget> actionTargets = [];

            public override string[] DefaultItemIdentifiers(NSToolbar toolbar)
            {
                return ToolbarItems;
            }

            public override string[] AllowedItemIdentifiers(NSToolbar toolbar)
            {
                return ToolbarItems;
            }

            public override NSToolbarItem WillInsertItem(NSToolbar toolbar, string itemIdentifier, bool willBeInserted)
            {
                return itemIdentifier switch
                {
                    SaveItem => CreateItem(SaveItem, "Save", "square.and.arrow.down", actions => actions.SaveAsync()),
                    UndoItem => CreateItem(UndoItem, "Undo", "arrow.uturn.backward", actions => actions.UndoAsync()),
                    RedoItem => CreateItem(RedoItem, "Redo", "arrow.uturn.forward", actions => actions.RedoAsync()),
                    PlayItem => CreateItem(PlayItem, "Play", "play.fill", actions =>
                    {
                        actions.RunWorld(false);
                        return Task.CompletedTask;
                    }),
                    DebugItem => CreateItem(DebugItem, "Debug", "ladybug.fill", actions =>
                    {
                        actions.RunWorld(true);
                        return Task.CompletedTask;
                    }),
                    TraceSceneItem => CreateItem(TraceSceneItem, "Trace Scene", "camera.metering.matrix", actions => actions.ExportPathTracedImageAsync(false)),
                    TraceSelectionItem => CreateItem(TraceSelectionItem, "Trace Selection", "scope", actions => actions.ExportPathTracedImageAsync(true)),
                    SaveLayoutItem => CreateItem(SaveLayoutItem, "Save Layout", "rectangle.3.group", actions => actions.SaveLayoutAsync()),
                    ResetLayoutItem => CreateItem(ResetLayoutItem, "Reset Layout", "arrow.counterclockwise", actions => actions.ResetLayoutAsync()),
                    SettingsItem => CreateItem(SettingsItem, "Settings", "gearshape", actions => actions.OpenSettingsAsync()),
                    _ => new NSToolbarItem(itemIdentifier)
                };
            }

            NSToolbarItem CreateItem(string identifier, string label, string symbolName, Func<EditorToolbarActions, Task> action)
            {
                var target = new ToolbarActionTarget(() => action(MauiProgram.GetService<EditorToolbarActions>()));
                actionTargets[identifier] = target;

                var barButton = CreateButton(label, symbolName, target);
                var item = NSToolbarItem.Create(identifier, barButton);
                item.Action = InvokeSelector;
                item.Target = target;
                item.Enabled = true;
                item.Label = label;
                item.PaletteLabel = label;
                item.ToolTip = label;
                item.Autovalidates = false;
                return item;
            }

            static UIBarButtonItem CreateButton(string label, string symbolName, NSObject target)
            {
                var image = UIImage.GetSystemImage(symbolName);
                return image != null
                    ? new UIBarButtonItem(image, UIBarButtonItemStyle.Plain, target, InvokeSelector)
                    : new UIBarButtonItem(label, UIBarButtonItemStyle.Plain, target, InvokeSelector);
            }

            sealed class ToolbarActionTarget : NSObject
            {
                readonly Func<Task> action;

                public ToolbarActionTarget(Func<Task> action)
                {
                    this.action = action;
                }

                [Export("invoke:")]
                public async void Invoke(NSObject sender)
                {
                    await action();
                }
            }
        }
    }
}
