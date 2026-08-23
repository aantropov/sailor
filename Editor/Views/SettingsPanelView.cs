using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.Runtime.CompilerServices;
using Microsoft.Maui.Controls.Shapes;
using SailorEditor.Services;
using SailorEditor.Settings;

namespace SailorEditor.Views;

public sealed class SettingsPanelView : ContentView
{
    const string GraphicsCategory = "Graphics";
    readonly UnifiedSettingsStore _store;
    readonly EditorSettingsPersistenceStore _persistence;
    readonly GraphicsSettingsService _graphicsSettings;
    readonly WorkspaceUiService _workspaceUiService;
    readonly Picker _scopePicker;
    readonly Entry _searchBar;
    readonly Label _statusLabel;
    readonly ObservableCollection<SettingsEditorRow> _rows = [];
    readonly List<GraphicsPresetEditor> _graphicsPresetEditors = [];
    readonly GraphicsSettingsDraftSession _graphicsDraftSession = new();
    readonly VerticalStackLayout _entriesLayout;
    Picker? _projectDefaultQualityPicker;
    Picker? _editorQualityPicker;
    Picker? _statsModePicker;
    GraphicsSettingsSnapshot? _graphicsSnapshot;
    bool _loaded;
    bool _graphicsDirty;
    bool _graphicsStale;
    bool _updatingGraphicsEditors;
    bool _applyingGraphics;
    bool _eventsSubscribed;

    public SettingsPanelView()
    {
        _store = MauiProgram.GetService<UnifiedSettingsStore>();
        _persistence = MauiProgram.GetService<EditorSettingsPersistenceStore>();
        _graphicsSettings = MauiProgram.GetService<GraphicsSettingsService>();
        _workspaceUiService = MauiProgram.GetService<WorkspaceUiService>();
        _scopePicker = new Picker
        {
            Title = "Category",
            ItemsSource = new[] { GraphicsCategory }
                .Concat(Enum.GetNames<SettingsScope>())
                .Cast<object>()
                .ToList(),
            SelectedItem = SettingsScope.Editor.ToString(),
            FontSize = 11,
            HeightRequest = 32,
            MinimumHeightRequest = 32,
            HorizontalOptions = LayoutOptions.Fill
        };
        _searchBar = new Entry
        {
            Placeholder = "Search settings",
            FontSize = 11,
            HeightRequest = 32,
            MinimumHeightRequest = 32,
            HorizontalOptions = LayoutOptions.Fill
        };
        _statusLabel = new Label
        {
            Margin = new Thickness(6, 1, 6, 2),
            FontSize = 10,
            Opacity = 0.8,
            LineBreakMode = LineBreakMode.WordWrap
        };
        _entriesLayout = new VerticalStackLayout
        {
            Spacing = 3,
            Padding = new Thickness(6, 1, 6, 8),
            HorizontalOptions = LayoutOptions.Fill
        };

        _scopePicker.SelectedIndexChanged += (_, _) => Refresh();
        _searchBar.TextChanged += (_, _) => Refresh();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;

        var actions = new HorizontalStackLayout
        {
            Spacing = 4,
            HorizontalOptions = LayoutOptions.End,
            Children =
            {
                CreateToolbarButton("Apply", async () => await ApplyAllAsync()),
                CreateToolbarButton("Revert", RevertAll),
                CreateToolbarButton("Reset", async () => await ResetScopeAsync())
            }
        };

        var toolbar = new Grid
        {
            Margin = new Thickness(6, 4, 6, 2),
            RowSpacing = 2,
            ColumnSpacing = 4,
            RowDefinitions =
            {
                new RowDefinition(GridLength.Auto),
                new RowDefinition(GridLength.Auto)
            },
            ColumnDefinitions =
            {
                new ColumnDefinition(new GridLength(2, GridUnitType.Star)),
                new ColumnDefinition(new GridLength(3, GridUnitType.Star))
            }
        };
        toolbar.Add(_scopePicker, 0, 0);
        toolbar.Add(_searchBar, 1, 0);
        toolbar.Add(actions, 0, 1);
        Grid.SetColumnSpan(actions, 2);

        var scroll = new ScrollView
        {
            Content = _entriesLayout,
            Orientation = ScrollOrientation.Vertical,
            VerticalOptions = LayoutOptions.Fill,
            HorizontalOptions = LayoutOptions.Fill,
            VerticalScrollBarVisibility = ScrollBarVisibility.Always
        };
        var root = new Grid
        {
            RowDefinitions =
            [
                new RowDefinition(GridLength.Auto),
                new RowDefinition(GridLength.Auto),
                new RowDefinition(GridLength.Star)
            ]
        };

        Grid.SetRow(toolbar, 0);
        Grid.SetRow(_statusLabel, 1);
        Grid.SetRow(scroll, 2);

        root.Children.Add(toolbar);
        root.Children.Add(_statusLabel);
        root.Children.Add(scroll);
        Content = root;

        Refresh();
    }

    async void OnLoaded(object? sender, EventArgs e)
    {
        SubscribeToEvents();

        if (!_loaded)
        {
            _loaded = true;
            await _persistence.LoadAsync(_store);
        }

        _graphicsSnapshot = await _graphicsSettings.EnsureLoadedAsync();
        _graphicsDraftSession.GetOrCreate(_graphicsSnapshot);
        Refresh();
        UpdateStatus();
    }

    void OnUnloaded(object? sender, EventArgs e)
    {
        CaptureGraphicsDraftFromEditors();
        UnsubscribeFromEvents();
    }

    void Refresh()
    {
        CaptureGraphicsDraftFromEditors();
        foreach (var row in _rows)
            row.PropertyChanged -= OnRowPropertyChanged;

        _rows.Clear();
        _entriesLayout.Children.Clear();

        if (IsGraphicsSelected())
        {
            RefreshGraphics();
            return;
        }

        _graphicsPresetEditors.Clear();
        _projectDefaultQualityPicker = null;
        _editorQualityPicker = null;
        _statsModePicker = null;

        var selectedScope = Enum.TryParse<SettingsScope>(
            _scopePicker.SelectedItem?.ToString(),
            out var scope)
            ? scope
            : SettingsScope.Editor;
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
        if (IsGraphicsSelected())
        {
            await ApplyGraphicsAsync();
            return;
        }

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
        if (IsGraphicsSelected())
        {
            _graphicsSnapshot = _graphicsSettings.Current ?? _graphicsSnapshot;
            if (_graphicsSnapshot is not null)
                _graphicsDraftSession.Replace(_graphicsSnapshot);
            _graphicsDirty = false;
            _graphicsStale = false;
            RefreshGraphics();
            UpdateStatus("Reverted pending graphics edits.");
            return;
        }

        foreach (var row in _rows)
            row.Revert();

        UpdateStatus("Reverted pending edits.");
    }

    async Task ResetScopeAsync()
    {
        if (IsGraphicsSelected())
        {
            var snapshot = _graphicsSnapshot ??
                await _graphicsSettings.EnsureLoadedAsync();
            try
            {
                _applyingGraphics = true;
                var result = await _graphicsSettings.ApplyAsync(
                    GraphicsSettingsDefaults.Project,
                    GraphicsSettingsDefaults.Editor,
                    snapshot);
                _graphicsSnapshot = _graphicsSettings.Current;
                if (_graphicsSnapshot is not null)
                    _graphicsDraftSession.Replace(_graphicsSnapshot);
                _graphicsDirty = false;
                _graphicsStale = false;
                RefreshGraphics();
                UpdateStatus(DescribeGraphicsApply(result, "Reset graphics settings to defaults."));
            }
            catch (Exception exception)
            {
                UpdateStatus($"Unable to reset graphics settings: {exception.Message}");
            }
            finally
            {
                _applyingGraphics = false;
            }
            return;
        }

        var selectedScope = Enum.TryParse<SettingsScope>(
            _scopePicker.SelectedItem?.ToString(),
            out var scope)
            ? scope
            : SettingsScope.Editor;
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

        if (IsGraphicsSelected())
        {
            if (_graphicsStale)
            {
                _statusLabel.Text = "Graphics settings changed outside this panel. Revert to reload before applying.";
            }
            else if (_graphicsDirty)
            {
                _statusLabel.Text = "Graphics settings have pending changes. Apply to validate, persist, and update the Engine.";
            }
            else if (_graphicsSnapshot?.Diagnostics.Count > 0)
            {
                _statusLabel.Text = string.Join(Environment.NewLine, _graphicsSnapshot.Diagnostics);
            }
            else
            {
                _statusLabel.Text = "No pending graphics changes.";
            }
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

    bool IsGraphicsSelected()
        => string.Equals(
            _scopePicker.SelectedItem?.ToString(),
            GraphicsCategory,
            StringComparison.Ordinal);

    void RefreshGraphics()
    {
        _entriesLayout.Children.Clear();
        _graphicsPresetEditors.Clear();
        _projectDefaultQualityPicker = null;
        _editorQualityPicker = null;
        _statsModePicker = null;

        var snapshot = _graphicsSnapshot ?? _graphicsSettings.Current;
        if (snapshot is null)
        {
            _entriesLayout.Children.Add(new Label
            {
                Text = "Graphics settings are loading…",
                Margin = new Thickness(8)
            });
            UpdateStatus();
            return;
        }

        _graphicsSnapshot = snapshot;
        var draft = _graphicsDraftSession.GetOrCreate(snapshot);
        _updatingGraphicsEditors = true;
        try
        {
            _entriesLayout.Children.Add(new Label
            {
                Text = "Project Graphics",
                FontSize = 14,
                FontAttributes = FontAttributes.Bold,
                Margin = new Thickness(0, 2, 0, 0)
            });
            var pathsLabel = new Label
            {
                Text = "ProjectSettings.yaml  •  Cache/EditorSettings.yaml",
                FontSize = 9,
                Opacity = 0.7,
                LineBreakMode = LineBreakMode.TailTruncation
            };
            ToolTipProperties.SetText(
                pathsLabel,
                $"Project: {snapshot.Paths.ProjectSettingsPath}\nEditor: {snapshot.Paths.EditorSettingsPath}");
            _entriesLayout.Children.Add(pathsLabel);

            _projectDefaultQualityPicker = CreateOptionPicker(
                QualityOptions,
                draft.ProjectDefaultQuality);
            _editorQualityPicker = CreateOptionPicker(
                EditorQualityOptions,
                draft.SelectedQuality);
            _statsModePicker = CreateOptionPicker(
                StatsModeOptions,
                draft.StatsMode);
            _entriesLayout.Children.Add(CreateGraphicsField(
                "Project Default Quality",
                "Preset used by standalone/game startup and by Project Default in the editor.",
                _projectDefaultQualityPicker));
            _entriesLayout.Children.Add(CreateGraphicsField(
                "Scene View Quality",
                "Workspace-local editor override. Applying a change restarts the Engine.",
                _editorQualityPicker));
            _entriesLayout.Children.Add(CreateGraphicsField(
                "Scene View Stats",
                "Workspace-local overlay mode. Applying a Stats-only change is live.",
                _statsModePicker));

            foreach (var quality in Enum.GetValues<GraphicsQualityLevel>())
            {
                if (!GraphicsSearchMatches(quality))
                    continue;

                var editor = new GraphicsPresetEditor(
                    quality,
                    draft.GetPreset(quality),
                    MarkGraphicsDirty);
                _graphicsPresetEditors.Add(editor);
                _entriesLayout.Children.Add(editor.CreateView(
                    initiallyExpanded: quality == snapshot.EffectiveQuality ||
                        !string.IsNullOrWhiteSpace(_searchBar.Text),
                    isActive: quality == snapshot.EffectiveQuality));
            }
        }
        finally
        {
            _updatingGraphicsEditors = false;
        }

        UpdateStatus();
    }

    async Task ApplyGraphicsAsync()
    {
        if (_graphicsStale)
        {
            UpdateStatus("Graphics settings changed outside this panel. Revert to reload before applying.");
            return;
        }

        var snapshot = _graphicsSnapshot ??
            await _graphicsSettings.EnsureLoadedAsync();
        CaptureGraphicsDraftFromEditors();
        var draft = _graphicsDraftSession.GetOrCreate(snapshot);
        if (!draft.TryBuild(
                out var project,
                out var editorSettings,
                out var issues))
        {
            UpdateStatus(string.Join(
                Environment.NewLine,
                issues.Select(issue => $"{issue.Path}: {issue.Message}")));
            return;
        }

        try
        {
            _applyingGraphics = true;
            var result = await _graphicsSettings.ApplyAsync(
                project,
                editorSettings,
                draft.SourceSnapshot);
            _graphicsSnapshot = _graphicsSettings.Current;
            if (_graphicsSnapshot is not null)
                _graphicsDraftSession.Replace(_graphicsSnapshot);
            _graphicsDirty = false;
            _graphicsStale = false;
            RefreshGraphics();
            UpdateStatus(DescribeGraphicsApply(result, "Graphics settings applied."));
        }
        catch (Exception exception)
        {
            if (_graphicsSettings.Current is { } current &&
                !ReferenceEquals(current, snapshot))
            {
                _graphicsSnapshot = current;
                _graphicsDraftSession.Replace(current);
                _graphicsDirty = false;
                _graphicsStale = false;
                RefreshGraphics();
                UpdateStatus($"Graphics settings were persisted, but the Engine update failed: {exception.Message}");
            }
            else
            {
                UpdateStatus($"Unable to apply graphics settings: {exception.Message}");
            }
        }
        finally
        {
            _applyingGraphics = false;
        }
    }

    void MarkGraphicsDirty()
    {
        if (_updatingGraphicsEditors)
            return;

        _graphicsDirty = true;
        UpdateStatus();
    }

    void CaptureGraphicsDraftFromEditors()
    {
        var draft = _graphicsDraftSession.Current;
        if (draft is null)
            return;

        if (_projectDefaultQualityPicker?.SelectedItem is PickerOption<GraphicsQualityLevel> projectQuality &&
            _editorQualityPicker?.SelectedItem is PickerOption<EditorQualitySelection> editorQuality &&
            _statsModePicker?.SelectedItem is PickerOption<GraphicsStatsMode> statsMode)
        {
            draft.SetSelections(
                projectQuality.Value,
                editorQuality.Value,
                statsMode.Value);
        }

        foreach (var editor in _graphicsPresetEditors)
            draft.SetPreset(editor.Quality, editor.CaptureDraft());
        _graphicsDirty = draft.IsDirty;
    }

    bool GraphicsSearchMatches(GraphicsQualityLevel quality)
    {
        if (string.IsNullOrWhiteSpace(_searchBar.Text))
            return true;

        var search = _searchBar.Text.Trim();
        return FormatQuality(quality).Contains(search, StringComparison.OrdinalIgnoreCase) ||
            "resolution msaa shadow cascades soft clouds sky lod bias graphics quality preset"
                .Contains(search, StringComparison.OrdinalIgnoreCase);
    }

    Picker CreateOptionPicker<T>(
        IReadOnlyList<PickerOption<T>> options,
        T selectedValue)
        where T : struct, Enum
    {
        var picker = new Picker
        {
            ItemsSource = options.Cast<object>().ToList(),
            SelectedItem = options.Single(option =>
                EqualityComparer<T>.Default.Equals(
                    option.Value,
                    selectedValue)),
            FontSize = 11,
            HeightRequest = 30,
            MinimumHeightRequest = 30,
            HorizontalOptions = LayoutOptions.Fill
        };
        picker.SelectedIndexChanged += (_, _) => MarkGraphicsDirty();
        return picker;
    }

    static View CreateGraphicsField(
        string title,
        string description,
        View editor)
    {
        var titleLabel = new Label
        {
            Text = title,
            FontSize = 11,
            FontAttributes = FontAttributes.Bold,
            LineBreakMode = LineBreakMode.TailTruncation,
            VerticalTextAlignment = TextAlignment.Center
        };
        ToolTipProperties.SetText(titleLabel, description);
        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition(new GridLength(6, GridUnitType.Star)),
                new ColumnDefinition(new GridLength(5, GridUnitType.Star))
            },
            ColumnSpacing = 6,
            Padding = new Thickness(0, 1)
        };
        grid.Add(titleLabel, 0, 0);
        grid.Add(editor, 1, 0);
        return grid;
    }

    void SubscribeToEvents()
    {
        if (_eventsSubscribed)
            return;

        _graphicsSettings.SettingsChanged += OnGraphicsSettingsChanged;
        _workspaceUiService.ProjectionChanged += OnWorkspaceProjectionChanged;
        _eventsSubscribed = true;
    }

    void UnsubscribeFromEvents()
    {
        if (!_eventsSubscribed)
            return;

        _graphicsSettings.SettingsChanged -= OnGraphicsSettingsChanged;
        _workspaceUiService.ProjectionChanged -= OnWorkspaceProjectionChanged;
        _eventsSubscribed = false;
    }

    void OnGraphicsSettingsChanged(
        object? sender,
        GraphicsSettingsSnapshot snapshot)
    {
        Dispatcher.Dispatch(() =>
        {
            if (_applyingGraphics)
            {
                _graphicsSnapshot = snapshot;
                return;
            }

            var workspaceChanged = _graphicsSnapshot is not null &&
                (_graphicsSnapshot.Paths != snapshot.Paths ||
                    _graphicsSnapshot.WorkspaceGeneration != snapshot.WorkspaceGeneration);
            if (_graphicsDirty && !workspaceChanged)
            {
                _graphicsStale = true;
                UpdateStatus();
                return;
            }

            _graphicsSnapshot = snapshot;
            _graphicsDraftSession.Replace(snapshot);
            _graphicsDirty = false;
            _graphicsStale = false;
            if (IsGraphicsSelected())
            {
                _entriesLayout.Children.Clear();
                RefreshGraphics();
            }
        });
    }

    void OnWorkspaceProjectionChanged(object? sender, EventArgs e)
        => _ = ReloadGraphicsForWorkspaceAsync();

    async Task ReloadGraphicsForWorkspaceAsync()
    {
        try
        {
            await _graphicsSettings.EnsureLoadedAsync();
        }
        catch (Exception exception)
        {
            Dispatcher.Dispatch(() =>
                UpdateStatus($"Unable to load graphics settings for the active workspace: {exception.Message}"));
        }
    }

    static string DescribeGraphicsApply(
        GraphicsSettingsApplyResult result,
        string successMessage)
    {
        if (result.QualityChanged && !result.EngineRestarted)
            return "Graphics settings were saved, but the Engine restart did not complete.";
        if (result.StatsChanged &&
            !result.QualityChanged &&
            !result.StatsAppliedLive)
        {
            return "Graphics settings were saved, but the live Stats command was rejected.";
        }
        return successMessage;
    }

    static string FormatQuality(GraphicsQualityLevel quality)
        => quality == GraphicsQualityLevel.VeryLow
            ? "Very Low"
            : quality.ToString();

    static readonly PickerOption<GraphicsQualityLevel>[] QualityOptions =
        Enum.GetValues<GraphicsQualityLevel>()
            .Select(value => new PickerOption<GraphicsQualityLevel>(
                value,
                FormatQuality(value)))
            .ToArray();

    static readonly PickerOption<EditorQualitySelection>[] EditorQualityOptions =
    [
        new(EditorQualitySelection.ProjectDefault, "Project Default"),
        new(EditorQualitySelection.Ultra, "Ultra"),
        new(EditorQualitySelection.High, "High"),
        new(EditorQualitySelection.Medium, "Medium"),
        new(EditorQualitySelection.Low, "Low"),
        new(EditorQualitySelection.VeryLow, "Very Low")
    ];

    static readonly PickerOption<GraphicsStatsMode>[] StatsModeOptions =
    [
        new(GraphicsStatsMode.None, "None"),
        new(GraphicsStatsMode.RenderStats, "Render stats"),
        new(GraphicsStatsMode.RenderStatsAndQueries, "Render stats + queries")
    ];

    View CreateRowView(SettingsEditorRow row)
    {
        var title = new Label { FontAttributes = FontAttributes.Bold };
        title.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.Title));
        title.BindingContext = row;

        var key = new Label { FontSize = 11, Opacity = 0.7 };
        key.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.Key));
        key.BindingContext = row;

        var source = new Label { FontSize = 11, Opacity = 0.7 };
        source.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.SourceText));
        source.BindingContext = row;

        var validation = new Label { FontSize = 11, TextColor = Colors.OrangeRed };
        validation.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.ValidationText));
        validation.BindingContext = row;

        var dirty = new Label { FontSize = 11, TextColor = Colors.Goldenrod };
        dirty.SetBinding(Label.TextProperty, nameof(SettingsEditorRow.DirtyText));
        dirty.BindingContext = row;

        ToolTipProperties.SetText(title, row.Description);

        var actions = new HorizontalStackLayout
        {
            Spacing = 4,
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
        var meta = new VerticalStackLayout
        {
            Spacing = 2,
            HorizontalOptions = LayoutOptions.Fill,
            Children = { title, key, source, dirty, validation }
        };

        var editorColumn = new VerticalStackLayout
        {
            Spacing = 3,
            HorizontalOptions = LayoutOptions.Fill,
            Children = { editor, actions }
        };

        var rowGrid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                new ColumnDefinition { Width = new GridLength(2, GridUnitType.Star) }
            },
            ColumnSpacing = 6,
            Padding = new Thickness(0, 2),
            HorizontalOptions = LayoutOptions.Fill
        };

        rowGrid.Add(meta, 0, 0);
        rowGrid.Add(editorColumn, 1, 0);

        return rowGrid;
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
        var toggle = new Switch
        {
            BindingContext = row,
            Scale = 0.8,
            HeightRequest = 30
        };
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
            ItemsSource = row.Definition.Entry.AllowedValues?.Cast<object>().ToList() ?? [],
            FontSize = 11,
            HeightRequest = 30,
            MinimumHeightRequest = 30
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
            IsPassword = row.Definition.Entry.IsSecret,
            FontSize = 11,
            HeightRequest = 30,
            MinimumHeightRequest = 30
        };
        entry.SetBinding(Entry.TextProperty, nameof(SettingsEditorRow.DraftText), mode: BindingMode.TwoWay);
        return entry;
    }

    static Button CreateToolbarButton(string text, Action action)
    {
        var button = new Button
        {
            Text = text,
            FontSize = 11,
            Padding = new Thickness(8, 2),
            HeightRequest = 28,
            MinimumHeightRequest = 28
        };
        button.Clicked += (_, _) => action();
        return button;
    }

    static Button CreateToolbarButton(string text, Func<Task> action)
    {
        var button = new Button
        {
            Text = text,
            FontSize = 11,
            Padding = new Thickness(8, 2),
            HeightRequest = 28,
            MinimumHeightRequest = 28
        };
        button.Clicked += async (_, _) => await action();
        return button;
    }

    sealed record PickerOption<T>(T Value, string DisplayName)
        where T : struct, Enum
    {
        public override string ToString() => DisplayName;
    }

    sealed class GraphicsPresetEditor
    {
        static readonly PickerOption<GraphicsShadowQuality>[] ShadowQualityOptions =
        [
            new(GraphicsShadowQuality.High, "High"),
            new(GraphicsShadowQuality.Medium, "Medium"),
            new(GraphicsShadowQuality.Low, "Low"),
            new(GraphicsShadowQuality.VeryLow, "Very Low")
        ];

        readonly Action _changed;
        readonly Entry _resolutionFactor;
        readonly Entry _fpsCap;
        readonly Picker _msaaSamples;
        readonly Picker _shadowQuality;
        readonly Entry _shadowBias;
        readonly Picker _shadowCascadeCount;
        readonly Entry _shadowCascadeResolutions;
        readonly Switch _supportSoftShadows;
        readonly Entry _cloudsResolutionMultiplier;
        readonly Entry _skyResolution;
        readonly Entry _vegetationInstanceBudget;
        readonly Entry _lodBias;

        public GraphicsPresetEditor(
            GraphicsQualityLevel quality,
            GraphicsQualityPresetDraft draft,
            Action changed)
        {
            Quality = quality;
            _changed = changed;
            _resolutionFactor = CreateEntry(draft.ResolutionFactor);
            _fpsCap = CreateEntry(draft.FpsCap);
            _msaaSamples = CreatePicker(
                new object[] { 1, 2, 4, 8 },
                draft.MsaaSamples);
            _shadowQuality = CreatePicker(
                ShadowQualityOptions.Cast<object>().ToArray(),
                ShadowQualityOptions.Single(x => x.Value == draft.ShadowQuality));
            _shadowBias = CreateEntry(draft.ShadowBias);
            _shadowCascadeCount = CreatePicker(
                new object[] { 1, 2, 3, 4 },
                draft.ShadowCascadeCount);
            _shadowCascadeResolutions = CreateEntry(
                draft.ShadowCascadeResolutions);
            _supportSoftShadows = new Switch
            {
                IsToggled = draft.SupportSoftShadows,
                Scale = 0.8,
                HeightRequest = 30,
                HorizontalOptions = LayoutOptions.End
            };
            _supportSoftShadows.Toggled += (_, _) => _changed();
            _cloudsResolutionMultiplier = CreateEntry(
                draft.CloudsResolutionMultiplier);
            _skyResolution = CreateEntry(draft.SkyResolution);
            _vegetationInstanceBudget = CreateEntry(
                draft.VegetationInstanceBudget);
            _lodBias = CreateEntry(draft.LodBias);
        }

        public GraphicsQualityLevel Quality { get; }

        public GraphicsQualityPresetDraft CaptureDraft()
            => new(
                _resolutionFactor.Text ?? string.Empty,
                _fpsCap.Text ?? string.Empty,
                _msaaSamples.SelectedItem is int msaa ? msaa : 0,
                _shadowQuality.SelectedItem is PickerOption<GraphicsShadowQuality> shadow
                    ? shadow.Value
                    : (GraphicsShadowQuality)(-1),
                _shadowBias.Text ?? string.Empty,
                _shadowCascadeCount.SelectedItem is int count ? count : 0,
                _shadowCascadeResolutions.Text ?? string.Empty,
                _supportSoftShadows.IsToggled,
                _cloudsResolutionMultiplier.Text ?? string.Empty,
                _skyResolution.Text ?? string.Empty,
                _vegetationInstanceBudget.Text ?? string.Empty,
                _lodBias.Text ?? string.Empty);

        public View CreateView(bool initiallyExpanded, bool isActive)
        {
            var fields = new VerticalStackLayout
            {
                Spacing = 1,
                Children =
                {
                    CreatePresetField("Resolution Factor", "0.25–2.0", _resolutionFactor),
                    CreatePresetField("FPS Cap", "Maximum CPU frames per second, 1–1000", _fpsCap),
                    CreatePresetField("MSAA", "Supported sample count", _msaaSamples),
                    CreatePresetField("Shadow Quality Cap", "Global cap over authored light quality", _shadowQuality),
                    CreatePresetField("Shadow Bias", "Vulkan constant depth bias, -16–16", _shadowBias),
                    CreatePresetField("Shadow Cascade Count", "Active directional cascades, 1–4", _shadowCascadeCount),
                    CreatePresetField("Cascade Resolutions", "Comma-separated powers of two, one per active cascade", _shadowCascadeResolutions),
                    CreatePresetField("Soft Shadows", "Enable soft shadow filtering", _supportSoftShadows),
                    CreatePresetField("Clouds Resolution Multiplier", "0.0625–2.0", _cloudsResolutionMultiplier),
                    CreatePresetField("Sky Resolution", "Power of two, 32–8192", _skyResolution),
                    CreatePresetField("Vegetation Instance Budget", "Global active grass instances, 0–1048576", _vegetationInstanceBudget),
                    CreatePresetField("LOD Bias", "Signed index shift, -8 (finer) to +8 (coarser)", _lodBias)
                }
            };

            var title = new Label
            {
                Text = $"{FormatQuality(Quality)} preset",
                FontSize = 12,
                FontAttributes = FontAttributes.Bold,
                VerticalTextAlignment = TextAlignment.Center
            };
            var state = new Label
            {
                Text = FormatPresetState(initiallyExpanded, isActive),
                FontSize = 10,
                Opacity = 0.7,
                HorizontalTextAlignment = TextAlignment.End,
                VerticalTextAlignment = TextAlignment.Center
            };
            var header = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition(GridLength.Star),
                    new ColumnDefinition(GridLength.Auto)
                },
                Padding = new Thickness(1, 0)
            };
            header.Add(title, 0, 0);
            header.Add(state, 1, 0);
            var tap = new TapGestureRecognizer();
            tap.Tapped += (_, _) =>
            {
                fields.IsVisible = !fields.IsVisible;
                state.Text = FormatPresetState(fields.IsVisible, isActive);
            };
            header.GestureRecognizers.Add(tap);
            fields.IsVisible = initiallyExpanded;

            return new Border
            {
                Stroke = Color.FromArgb("#3A3A3A"),
                StrokeThickness = 1,
                StrokeShape = new RoundRectangle { CornerRadius = 3 },
                Padding = new Thickness(6, 4),
                Margin = new Thickness(0, 1),
                Content = new VerticalStackLayout
                {
                    Spacing = 2,
                    Children =
                    {
                        header,
                        fields
                    }
                }
            };
        }

        static string FormatPresetState(bool expanded, bool isActive)
            => $"{(expanded ? "▾" : "▸")}{(isActive ? "  Active" : string.Empty)}";

        public bool TryBuild(
            out GraphicsQualityPresetSettings settings,
            ICollection<GraphicsSettingsValidationIssue> issues)
        {
            var path = $"graphics.presets.{Quality}";
            var valid = true;
            valid &= TryParseDouble(
                _resolutionFactor.Text,
                $"{path}.resolutionFactor",
                "Resolution factor",
                issues,
                out var resolutionFactor);
            valid &= TryParseInt(
                _fpsCap.Text,
                $"{path}.fpsCap",
                "FPS cap",
                issues,
                out var fpsCap);
            valid &= TryParseDouble(
                _shadowBias.Text,
                $"{path}.shadowBias",
                "Shadow bias",
                issues,
                out var shadowBias);
            valid &= TryParseCascadeResolutions(
                _shadowCascadeResolutions.Text,
                $"{path}.shadowCascadeResolutions",
                issues,
                out var cascadeResolutions);
            valid &= TryParseDouble(
                _cloudsResolutionMultiplier.Text,
                $"{path}.cloudsResolutionMultiplier",
                "Clouds resolution multiplier",
                issues,
                out var cloudsResolutionMultiplier);
            valid &= TryParseInt(
                _skyResolution.Text,
                $"{path}.skyResolution",
                "Sky resolution",
                issues,
                out var skyResolution);
            valid &= TryParseInt(
                _vegetationInstanceBudget.Text,
                $"{path}.vegetationInstanceBudget",
                "Vegetation instance budget",
                issues,
                out var vegetationInstanceBudget);
            valid &= TryParseInt(
                _lodBias.Text,
                $"{path}.lodBias",
                "LOD bias",
                issues,
                out var lodBias);

            var msaaSamples = _msaaSamples.SelectedItem is int msaa
                ? msaa
                : 0;
            var shadowQuality =
                _shadowQuality.SelectedItem is PickerOption<GraphicsShadowQuality> shadow
                    ? shadow.Value
                    : (GraphicsShadowQuality)(-1);
            var cascadeCount =
                _shadowCascadeCount.SelectedItem is int count
                    ? count
                    : 0;
            settings = new GraphicsQualityPresetSettings
            {
                ResolutionFactor = resolutionFactor,
                FpsCap = fpsCap,
                MsaaSamples = msaaSamples,
                ShadowQuality = shadowQuality,
                ShadowBias = shadowBias,
                ShadowCascadeCount = cascadeCount,
                ShadowCascadeResolutions = cascadeResolutions,
                SupportSoftShadows = _supportSoftShadows.IsToggled,
                CloudsResolutionMultiplier = cloudsResolutionMultiplier,
                SkyResolution = skyResolution,
                VegetationInstanceBudget = vegetationInstanceBudget,
                LodBias = lodBias
            };
            return valid;
        }

        Entry CreateEntry(string text)
        {
            var entry = new Entry
            {
                Text = text,
                Keyboard = Keyboard.Text,
                FontSize = 11,
                HeightRequest = 30,
                MinimumHeightRequest = 30,
                HorizontalOptions = LayoutOptions.Fill
            };
            entry.TextChanged += (_, _) => _changed();
            return entry;
        }

        Picker CreatePicker(
            IReadOnlyList<object> options,
            object selected)
        {
            var picker = new Picker
            {
                ItemsSource = options.ToList(),
                SelectedItem = selected,
                FontSize = 11,
                HeightRequest = 30,
                MinimumHeightRequest = 30,
                HorizontalOptions = LayoutOptions.Fill
            };
            picker.SelectedIndexChanged += (_, _) => _changed();
            return picker;
        }

        static View CreatePresetField(
            string title,
            string description,
            View editor)
        {
            var titleLabel = new Label
            {
                Text = title,
                FontSize = 10,
                LineBreakMode = LineBreakMode.TailTruncation,
                VerticalTextAlignment = TextAlignment.Center
            };
            ToolTipProperties.SetText(titleLabel, description);
            var grid = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition(new GridLength(6, GridUnitType.Star)),
                    new ColumnDefinition(new GridLength(5, GridUnitType.Star))
                },
                ColumnSpacing = 6,
                Padding = new Thickness(0, 0)
            };
            grid.Add(titleLabel, 0, 0);
            grid.Add(editor, 1, 0);
            return grid;
        }

        static bool TryParseDouble(
            string? text,
            string path,
            string displayName,
            ICollection<GraphicsSettingsValidationIssue> issues,
            out double value)
        {
            if (double.TryParse(
                    text,
                    NumberStyles.Float,
                    CultureInfo.InvariantCulture,
                    out value) &&
                double.IsFinite(value))
            {
                return true;
            }

            issues.Add(new GraphicsSettingsValidationIssue(
                path,
                $"{displayName} must be a finite number using '.' as the decimal separator."));
            value = 0;
            return false;
        }

        static bool TryParseInt(
            string? text,
            string path,
            string displayName,
            ICollection<GraphicsSettingsValidationIssue> issues,
            out int value)
        {
            if (int.TryParse(
                    text,
                    NumberStyles.Integer,
                    CultureInfo.InvariantCulture,
                    out value))
            {
                return true;
            }

            issues.Add(new GraphicsSettingsValidationIssue(
                path,
                $"{displayName} must be an integer."));
            value = 0;
            return false;
        }

        static bool TryParseCascadeResolutions(
            string? text,
            string path,
            ICollection<GraphicsSettingsValidationIssue> issues,
            out IReadOnlyList<int> values)
        {
            var parsed = new List<int>();
            var parts = (text ?? string.Empty).Split(
                ',',
                StringSplitOptions.TrimEntries);
            if (parts.Length == 0 || parts.Any(string.IsNullOrWhiteSpace))
            {
                issues.Add(new GraphicsSettingsValidationIssue(
                    path,
                    "Cascade resolutions must be a comma-separated list of integers."));
                values = [];
                return false;
            }

            foreach (var part in parts)
            {
                if (!int.TryParse(
                        part,
                        NumberStyles.Integer,
                        CultureInfo.InvariantCulture,
                        out var value))
                {
                    issues.Add(new GraphicsSettingsValidationIssue(
                        path,
                        $"Cascade resolution '{part}' is not an integer."));
                    values = [];
                    return false;
                }
                parsed.Add(value);
            }

            values = parsed;
            return true;
        }

        static string FormatDouble(double value)
            => value.ToString("0.####", CultureInfo.InvariantCulture);
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
