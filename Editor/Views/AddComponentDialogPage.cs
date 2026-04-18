using System.Collections.ObjectModel;

namespace SailorEditor.Views;

public sealed class AddComponentDialogPage : ContentPage
{
    readonly TaskCompletionSource<string> completion = new();
    readonly List<string> componentTypes;
    readonly ObservableCollection<string> filteredTypes = [];
    readonly ListView listView;
    string selectedType;

    AddComponentDialogPage(IEnumerable<string> componentTypes)
    {
        this.componentTypes = componentTypes.ToList();

        Title = "Add Component";
        BackgroundColor = Colors.Black;

        var searchBar = new SearchBar
        {
            Placeholder = "Filter by component name",
            Margin = new Thickness(12, 12, 12, 4)
        };

        listView = new ListView
        {
            ItemsSource = filteredTypes,
            SelectionMode = ListViewSelectionMode.Single,
            Margin = new Thickness(12, 4),
            HeightRequest = 420
        };

        listView.ItemSelected += (sender, args) =>
        {
            selectedType = args.SelectedItem as string;
        };

        listView.ItemTapped += async (sender, args) =>
        {
            if (args.Item is string typeName)
            {
                await Complete(typeName);
            }
        };

        searchBar.TextChanged += (sender, args) => ApplyFilter(args.NewTextValue);

        var okButton = new Button { Text = "OK", WidthRequest = 120 };
        okButton.Clicked += async (sender, args) => await Complete(selectedType);

        var cancelButton = new Button { Text = "Cancel", WidthRequest = 120 };
        cancelButton.Clicked += async (sender, args) => await Complete(null);

        Content = new VerticalStackLayout
        {
            Padding = new Thickness(16),
            WidthRequest = 620,
            Spacing = 8,
            Children =
            {
                searchBar,
                listView,
                new HorizontalStackLayout
                {
                    HorizontalOptions = LayoutOptions.End,
                    Spacing = 8,
                    Children = { cancelButton, okButton }
                }
            }
        };

        ApplyFilter(string.Empty);
        searchBar.Focus();
    }

    public static async Task<string> ShowAsync(IEnumerable<string> componentTypes)
    {
        var page = new AddComponentDialogPage(componentTypes);
        await Application.Current.MainPage.Navigation.PushModalAsync(page);
        return await page.completion.Task;
    }

    void ApplyFilter(string filter)
    {
        filteredTypes.Clear();

        var normalized = filter?.Trim() ?? string.Empty;
        foreach (var typeName in componentTypes.Where(typeName =>
            string.IsNullOrEmpty(normalized) ||
            typeName.Contains(normalized, StringComparison.OrdinalIgnoreCase)))
        {
            filteredTypes.Add(typeName);
        }

        selectedType = filteredTypes.FirstOrDefault();
        listView.SelectedItem = selectedType;
    }

    async Task Complete(string componentTypeName)
    {
        if (completion.Task.IsCompleted)
        {
            return;
        }

        completion.TrySetResult(componentTypeName);
        await Application.Current.MainPage.Navigation.PopModalAsync();
    }
}
