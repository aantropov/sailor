using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.Runtime.CompilerServices;
using Microsoft.Maui.Controls.Shapes;
using SailorEditor.Settings;

namespace SailorEditor.Views;

public sealed class SettingsPanelView : ContentView
{
    readonly UnifiedSettingsStore _store;
    readonly EditorSettingsPersistenceStore _persistence;
    readonly Picker _scopePicker;
    readonly SearchBar _searchBar;
    readonly Label _statusLabel;
    readonly ObservableCollection<SettingsEditorRow> _rows = [];
    readonly VerticalStackLayout _entriesLayout;
    bool _loaded;

    public SettingsPanelView()
    {
        _store = MauiProgram.GetService<UnifiedSettingsStore>();
        _persistence = MauiProgram.GetService<EditorSettingsPersistenceStore>();
        _scopePicker = new Picker { Title = "Category", ItemsSource = Enum.GetValues<SettingsScope>().Cast<object>().ToList(), SelectedIndex = 0 };
        _searchBar = new SearchBar { Placeholder = "Search settings" };
        _statusLabel = new Label { Margin = new Thickness(8, 0), FontSize = 11, Opacity = 0.8 };
        _entriesLayout = new VerticalStackLayout { Spacing = 8, Padding = new Thickness(8, 0, 8, 8) };

        _scopePicker.SelectedIndexChanged += (_, _) => Refresh();
        _searchBar.TextChanged += (_, _) => Refresh();
        Loaded += OnLoaded;

        var title = new Label { Text = "Settings", FontAttributes = FontAttributes.Bold, Margin = new Thickness(8, 8, 8, 0) };
        var toolbar = new HorizontalStackLayout
        {
            Margin = new Thickness(8, 4),
            Spacing = 8,
            Children =
            {
                _scopePicker,
                CreateToolbarButton("Apply", async () => await ApplyAllAsync()),
                CreateToolbarButton("Revert", RevertAll),
                CreateToolbarButton("Reset Scope", async () => await ResetScopeAsync())
            }
        };

        var scroll = new ScrollView { Content = _entriesLayout };
        var root = new Grid
        {
            RowDefinitions =
            [
                new RowDefinition(GridLength.Auto),
                new RowDefinition(GridLength.Auto),
                new RowDefinition(GridLength.Auto),
                new RowDefinition(GridLength.Auto),
                new RowDefinition(GridLength.Star)
            ]
        };

        Grid.SetRow(toolbar, 1);
        Grid.SetRow(_searchBar, 2);
        Grid.SetRow(_statusLabel, 3);
        Grid.SetRow(scroll, 4);

        root.Children.Add(title);
        root.Children.Add(toolbar);
        root.Children.Add(_searchBar);
        root.Children.Add(_statusLabel);
        root.Children.Add(scroll);
        Content = root;

        Refresh();
    }

    async void OnLoaded(object? sender, EventArgs e)
    {
        if (_loaded)
            return;

        _loaded = true;
        await _persistence.LoadAsync(_store);
        Refresh();
        UpdateStatus();
    }

    void Refresh()
    {
        foreach (var row in _rows)
            row.PropertyChanged -= OnRowPropertyChanged;

        _rows.Clear();
        _entriesLayout.Children.Clear();

        var selectedScope = _scopePicker.SelectedItem is SettingsScope scope ? scope : SettingsScope.Project;
        var rows = EditorSettingsCatalog.Definitions
            .Where(definition => definition.Entry.Scope == selectedScope)
            .Where(definition => string.IsNullOrWhiteSpace(_searchBar.Text)
                || definition.Entry.DisplayName.Contains(_searchBar.Text, StringComparison.OrdinalIgnoreCase)
                || definition.Entry.Key.Contains(_searchBar.Text, StringComparison.OrdinalIgnoreCase)
                || (definition.Entry.Description?.Contains(_searchBar.Text, StringComparison.OrdinalIgnoreCase) ?? false))
            .Select(definition => new SettingsEditorRow(_store, definition))
            .ToArray();

        foreach (var row in rows)
        {
            row.PropertyChanged += OnRowPropertyChanged;
            _rows.Add(row);
            _entriesLayout.Children.Add(CreateRowView(row));
        }

        if (_rows.Count == 0)
        {
            _entriesLayout.Children.Add(new Label
            {
                Text = "No settings match this filter.",
                Margin = new Thickness(8),
                Opacity = 0.7
            });
        }

        UpdateStatus();
    }

    void OnRowPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(SettingsEditorRow.IsDirty) or nameof(SettingsEditorRow.ValidationText) or nameof(SettingsEditorRow.HasErrors))
            UpdateStatus();
    }

    async Task ApplyAllAsync()
    {
        var dirtyRows = _rows.Where(x => x.IsDirty).ToArray();
        var invalidRows = dirtyRows.Where(x => x.HasErrors).ToArray();
        if (invalidRows.Length > 0)
        {
            _statusLabel.Text = $"Fix validation errors in {invalidRows.Length} setting(s) before applying.";
            return;
        }

        foreach (var row in dirtyRows)
            await row.ApplyAsync();

        await _persistence.SaveAsync(_store);
        UpdateStatus($"Applied {dirtyRows.Length} setting(s).");
    }

    void RevertAll()
    {
        foreach (var row in _rows)
            row.Revert();

        UpdateStatus("Reverted pending edits.");
    }

    async Task ResetScopeAsync()
    {
        var selectedScope = _scopePicker.SelectedItem is SettingsScope scope ? scope : SettingsScope.Project;
        _store.ResetScope(selectedScope);
        await _persistence.SaveAsync(_store);
        Refresh();
        UpdateStatus($"Reset {selectedScope} settings to defaults.");
    }

    void UpdateStatus(string? overrideText = null)
    {
        if (!string.IsNullOrWhiteSpace(overrideText))
        {
            _statusLabel.Text = overrideText;
            return;
        }

        var dirtyCount = _rows.Count(x => x.IsDirty);
        var errorCount = _rows.Count(x => x.HasErrors);
        _statusLabel.Text = dirtyCount == 0
            ? "No pending changes."
            : errorCount == 0
                ? $"{dirtyCount} pending change(s). Apply to persist them."
                : $"{dirtyCount} pending change(s), {errorCount} with validation errors.";
    }

    View CreateRowView(SettingsEditorRow row)
    {
        var title = new Label { FontAttributes = FontAttributes.Bold };
        title.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.Title));
        title.BindingContext = row;

        var key = new Label { FontSize = 11, Opacity = 0.7 };
        key.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.Key));
        key.BindingContext = row;

        var description = new Label { FontSize = 12, Opacity = 0.85 };
        description.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.Description));
        description.BindingContext = row;

        var source = new Label { FontSize = 11, Opacity = 0.7 };
        source.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.SourceText));
        source.BindingContext = row;

        var validation = new Label { FontSize = 11, TextColor = Colors.OrangeRed };
        validation.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.ValidationText));
        validation.BindingContext = row;

        var dirty = new Label { FontSize = 11, TextColor = Colors.Goldenrod };
        dirty.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.DirtyText));
        dirty.BindingContext = row;

        var actions = new HorizontalStackLayout
        {
            Spacing = 8,
            Children =
            {
                CreateToolbarButton("Apply", async () =>
                {
                    if (row.HasErrors)
                    {
                        UpdateStatus($"Fix '{row.Title}' before applying.");
                        return;
                    }

                    await row.ApplyAsync();
                    await _persistence.SaveAsync(_store);
                    UpdateStatus($"Applied '{row.Title}'.");
                }),
                CreateToolbarButton("Revert", () =>
                {
                    row.Revert();
                    UpdateStatus($"Reverted '{row.Title}'.");
                }),
                CreateToolbarButton("Reset", async () =>
                {
                    row.ResetToDefault();
                    await row.ApplyAsync();
                    await _persistence.SaveAsync(_store);
                    UpdateStatus($"Reset '{row.Title}' to default.");
                })
            }
        };

        var editor = CreateEditor(row);
        var meta = new VerticalStackLayout { Spacing = 2, Children = { title, key, description, source, dirty, validation } };

        return new Border
        {
            StrokeThickness = 1,
            StrokeShape = new RoundRectangle { CornerRadius = 6 },
            Padding = new Thickness(10, 8),
            Content = new VerticalStackLayout { Spacing = 8, Children = { meta, editor, actions } }
        };
    }

    View CreateEditor(SettingsEditorRow row)
    {
        return row.Definition.Entry.ValueKind switch
        {
            SettingsValueKind.Boolean => CreateBooleanEditor(row),
            SettingsValueKind.Enum => CreateEnumEditor(row),
            SettingsValueKind.Integer => CreateTextEditor(row, Keyboard.Numeric),
            SettingsValueKind.Float => CreateTextEditor(row, Keyboard.Numeric),
            SettingsValueKind.String => CreateTextEditor(row, Keyboard.Text),
            SettingsValueKind.Path => CreateTextEditor(row, Keyboard.Text, "Relative or absolute path"),
            _ => new Label { Text = "Unsupported editor." }
        };
    }

    View CreateBooleanEditor(SettingsEditorRow row)
    {
        var toggle = new Switch { BindingContext = row };
        toggle.SetBinding(Switch.IsToggledProperty, nameof(SettingsEditorRow.BooleanValue), mode: BindingMode.TwoWay);

        return new HorizontalStackLayout
        {
            Spacing = 8,
            Children =
            {
                toggle,
                new Label { Text = "Enabled", VerticalTextAlignment = TextAlignment.Center }
            }
        };
    }

    View CreateEnumEditor(SettingsEditorRow row)
    {
        var picker = new Picker
        {
            BindingContext = row,
            ItemsSource = row.Definition.Entry.AllowedValues?.Cast<object>().ToList() ?? []
        };
        picker.SetBinding(Picker.SelectedItemProperty, nameof(SettingsEditorRow.DraftText), mode: BindingMode.TwoWay);
        return picker;
    }

    View CreateTextEditor(SettingsEditorRow row, Keyboard keyboard, string? placeholder = null)
    {
        var entry = new Entry
        {
            BindingContext = row,
            Keyboard = keyboard,
            Placeholder = placeholder,
            IsPassword = row.Definition.Entry.IsSecret
        };
        entry.SetBinding(Entry.TextProperty, nameof(SettingsEditorRow.DraftText), mode: BindingMode.TwoWay);
        return entry;
    }

    static Button CreateToolbarButton(string text, Action action)
    {
        var button = new Button { Text = text, Padding = new Thickness(12, 6) };
        button.Clicked += (_, _) => action();
        return button;
    }

    static Button CreateToolbarButton(string text, Func<Task> action)
    {
        var button = new Button { Text = text, Padding = new Thickness(12, 6) };
        button.Clicked += async (_, _) => await action();
        return button;
    }

    sealed class SettingsEditorRow : INotifyPropertyChanged
    {
        readonly UnifiedSettingsStore _store;
        object? _draftValue;
        SettingsValidationResult _validation = SettingsValidationResult.Success;
        string _sourceText = string.Empty;
        bool _isDirty;
        bool _resetToDefault;

        public SettingsEditorRow(UnifiedSettingsStore store, SettingsDefinition definition)
        {
            _store = store;
            Definition = definition;
            Title = definition.Entry.DisplayName;
            Key = definition.Entry.Key;
            Description = definition.Entry.Description ?? string.Empty;
            RebindFromStore();
        }

        public event PropertyChangedEventHandler? PropertyChanged;

        public SettingsDefinition Definition { get; }
        public string Title { get; }
        public string Key { get; }
        public string Description { get; }
        public string SourceText
        {
            get => _sourceText;
            private set => SetField(ref _sourceText, value);
        }

        public string DraftText
        {
            get => _draftValue switch
            {
                null => string.Empty,
                double d => d.ToString("0.###", CultureInfo.InvariantCulture),
                _ => Convert.ToString(_draftValue, CultureInfo.InvariantCulture) ?? string.Empty
            };
            set => SetDraftValue(value);
        }

        public bool BooleanValue
        {
            get => _draftValue as bool? ?? false;
            set => SetDraftValue(value);
        }
        public bool HasErrors => !_validation.IsValid;
        public string ValidationText => string.Join(Environment.NewLine, _validation.Messages.Select(x => x.Message));
        public bool IsDirty
        {
            get => _isDirty;
            private set
            {
                if (SetField(ref _isDirty, value))
                    OnPropertyChanged(nameof(DirtyText));
            }
        }

        public string DirtyText => IsDirty ? "Modified" : string.Empty;

        public void SetDraftText(string? text) => SetDraftValue(text);

        public void SetDraftValue(object? value)
        {
            _resetToDefault = false;
            _draftValue = value;
            _validation = _store.Validate(Definition.Entry, value);
            IsDirty = !Equals(_store.GetStoredValue(Definition.Entry, Definition.Entry.Scope), _store.NormalizeForStorage(Definition.Entry, value));
            NotifyStateChanged();
        }

        public async Task ApplyAsync()
        {
            if (_resetToDefault)
                _store.ResetValue(Definition.Entry, Definition.Entry.Scope);
            else
                await _store.SetValueAsync(Definition.Entry, Definition.Entry.Scope, _draftValue);

            RebindFromStore();
        }

        public void Revert() => RebindFromStore();

        public void ResetToDefault()
        {
            _resetToDefault = true;
            _draftValue = Definition.DefaultValue;
            _validation = _store.Validate(Definition.Entry, _draftValue);
            IsDirty = _store.GetStoredValue(Definition.Entry, Definition.Entry.Scope) is not null;
            NotifyStateChanged();
        }

        void RebindFromStore()
        {
            _resetToDefault = false;
            _draftValue = _store.GetStoredValue(Definition.Entry, Definition.Entry.Scope) ?? Definition.DefaultValue;
            _validation = _store.Validate(Definition.Entry, _draftValue);
            var effective = _store.Resolve(Definition.Entry);
            SourceText = effective.SourceScope is { } source
                ? $"Resolved from {source} scope"
                : "Using catalog default";
            IsDirty = false;
            NotifyStateChanged();
        }

        void NotifyStateChanged()
        {
            OnPropertyChanged(nameof(DraftText));
            OnPropertyChanged(nameof(BooleanValue));
            OnPropertyChanged(nameof(ValidationText));
            OnPropertyChanged(nameof(HasErrors));
        }

        bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
        {
            if (EqualityComparer<T>.Default.Equals(field, value))
                return false;

            field = value;
            OnPropertyChanged(propertyName);
            return true;
        }

        void OnPropertyChanged([CallerMemberName] string? propertyName = null)
            => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
