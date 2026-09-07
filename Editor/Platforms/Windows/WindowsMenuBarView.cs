using Microsoft.Maui.Handlers;
using Microsoft.UI.Xaml;
using SolidColorBrush = Microsoft.UI.Xaml.Media.SolidColorBrush;
using Native = Microsoft.UI.Xaml.Controls;

namespace SailorEditor.Platforms.Windows;

public sealed class WindowsMenuBarView(IList<MenuBarItem> items) : View
{
    public IList<MenuBarItem> Items { get; } = items;
}

public sealed class WindowsMenuBarHandler : ViewHandler<WindowsMenuBarView, Native.MenuBar>
{
    public WindowsMenuBarHandler() : base(ViewMapper) { }

    protected override Native.MenuBar CreatePlatformView() => new()
    {
        RequestedTheme = ElementTheme.Dark,
        Background = new SolidColorBrush(global::Windows.UI.Color.FromArgb(255, 32, 35, 40))
    };

    protected override void ConnectHandler(Native.MenuBar platformView)
    {
        base.ConnectHandler(platformView);
        foreach (var menu in VirtualView.Items)
        {
            var nativeMenu = new Native.MenuBarItem { Title = menu.Text };
            foreach (var item in menu)
                nativeMenu.Items.Add(CreateItem(item));
            platformView.Items.Add(nativeMenu);
        }
    }

    protected override void DisconnectHandler(Native.MenuBar platformView)
    {
        platformView.Items.Clear();
        base.DisconnectHandler(platformView);
    }

    static Native.MenuFlyoutItemBase CreateItem(IMenuElement item)
    {
        if (item is MenuFlyoutSeparator)
            return new Native.MenuFlyoutSeparator();
        if (item is MenuFlyoutSubItem submenu)
        {
            var nativeSubmenu = new Native.MenuFlyoutSubItem { Text = submenu.Text };
            foreach (var child in submenu)
                nativeSubmenu.Items.Add(CreateItem(child));
            return nativeSubmenu;
        }

        var nativeItem = new Native.MenuFlyoutItem
        {
            Text = item.Text,
            IsEnabled = item.IsEnabled
        };
        nativeItem.Click += (_, _) => item.Clicked();
        return nativeItem;
    }
}
