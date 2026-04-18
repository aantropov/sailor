using System.Windows.Input;

namespace SailorEditor.Services;

public class EditorContextMenuItem
{
    public string Text { get; init; } = string.Empty;
    public ICommand Command { get; init; }
    public object CommandParameter { get; init; }
    public bool IsEnabled { get; init; } = true;
    public bool IsDestructive { get; init; } = false;
}

public class EditorContextMenuService
{
    public MenuFlyout CreateFlyout(params EditorContextMenuItem[] items)
    {
        var flyout = new MenuFlyout();
        foreach (var item in items.Where(item => item != null))
        {
            flyout.Add(new MenuFlyoutItem
            {
                Text = item.Text,
                Command = item.Command,
                CommandParameter = item.CommandParameter,
                IsEnabled = item.IsEnabled
            });
        }

        return flyout;
    }

    public async void Show(params EditorContextMenuItem[] items)
    {
        var availableItems = items
            .Where(item => item != null && item.IsEnabled && item.Command != null && item.Command.CanExecute(item.CommandParameter))
            .ToList();

        if (availableItems.Count == 0)
        {
            return;
        }

        var page = Application.Current?.MainPage;
        if (page == null)
        {
            return;
        }

        var selected = await page.DisplayActionSheet(null, "Cancel", null, availableItems.Select(item => item.Text).ToArray());
        var selectedItem = availableItems.FirstOrDefault(item => item.Text == selected);
        selectedItem?.Command.Execute(selectedItem.CommandParameter);
    }
}
