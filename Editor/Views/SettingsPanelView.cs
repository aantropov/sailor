using SailorEditor.Settings;

namespace SailorEditor.Views;

public sealed class SettingsPanelView : ContentView
{
    readonly UnifiedSettingsStore _store;
    readonly Picker _scopePicker;
    readonly SearchBar _searchBar;
    readonly CollectionView _entriesView;

    public SettingsPanelView()
    {
        _store = MauiProgram.GetService<UnifiedSettingsStore>();
        _scopePicker = new Picker { Title = "Scope", ItemsSource = Enum.GetValues<SettingsScope>().Cast<object>().ToList(), SelectedIndex = 0 };
        _searchBar = new SearchBar { Placeholder = "Search settings" };
        _entriesView = new CollectionView
        {
            SelectionMode = SelectionMode.None,
            ItemTemplate = new DataTemplate(() =>
            {
                var name = new Label { FontAttributes = FontAttributes.Bold };
                name.SetBinding(Label.TextProperty, nameof(SettingsPanelRow.Title));
                var key = new Label { FontSize = 11, Opacity = 0.7 };
                key.SetBinding(Label.TextProperty, nameof(SettingsPanelRow.Key));
                var value = new Label();
                value.SetBinding(Label.TextProperty, nameof(SettingsPanelRow.ValueText));
                var scope = new Label { FontSize = 11, Opacity = 0.7 };
                scope.SetBinding(Label.TextProperty, nameof(SettingsPanelRow.SourceText));

                return new Border
                {
                    StrokeThickness = 1,
                    StrokeShape = new RoundRectangle { CornerRadius = 6 },
                    Padding = new Thickness(10, 8),
                    Margin = new Thickness(8, 4),
                    Content = new VerticalStackLayout { Spacing = 2, Children = { name, key, value, scope } }
                };
            })
        };

        _scopePicker.SelectedIndexChanged += (_, _) => Refresh();
        _searchBar.TextChanged += (_, _) => Refresh();

        var root = new Grid { RowDefinitions = [new RowDefinition(GridLength.Auto), new RowDefinition(GridLength.Auto), new RowDefinition(GridLength.Star)] };
        var title = new Label { Text = "Settings", FontAttributes = FontAttributes.Bold, Margin = new Thickness(8, 8, 8, 0) };
        var scopeRow = new HorizontalStackLayout { Margin = new Thickness(8, 4), Spacing = 8, Children = { _scopePicker } };
        var contentGrid = new Grid { RowDefinitions = [new RowDefinition(GridLength.Auto), new RowDefinition(GridLength.Star)] };

        Grid.SetRow(scopeRow, 1);
        Grid.SetRow(contentGrid, 2);
        Grid.SetRow(_entriesView, 1);

        contentGrid.Children.Add(_searchBar);
        contentGrid.Children.Add(_entriesView);
        root.Children.Add(title);
        root.Children.Add(scopeRow);
        root.Children.Add(contentGrid);
        Content = root;

        Refresh();
    }

    void Refresh()
    {
        var selectedScope = _scopePicker.SelectedItem is SettingsScope scope ? scope : SettingsScope.Project;
        var rows = EditorSettingsCatalog.Definitions
            .Where(definition => definition.Entry.Scope == selectedScope)
            .Select(definition =>
            {
                var effective = _store.Resolve(definition.Entry);
                return new SettingsPanelRow(
                    definition.Entry.Key,
                    definition.Entry.DisplayName,
                    effective.Value?.ToString() ?? "<default>",
                    effective.SourceScope is { } source ? $"Resolved from {source}" : "Using default value");
            })
            .Where(row => string.IsNullOrWhiteSpace(_searchBar.Text)
                || row.Title.Contains(_searchBar.Text, StringComparison.OrdinalIgnoreCase)
                || row.Key.Contains(_searchBar.Text, StringComparison.OrdinalIgnoreCase))
            .ToArray();

        _entriesView.ItemsSource = rows;
    }

    sealed record SettingsPanelRow(string Key, string Title, string ValueText, string SourceText);
}
