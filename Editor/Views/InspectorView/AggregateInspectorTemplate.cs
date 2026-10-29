using CommunityToolkit.Mvvm.Input;
using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Views;
using SailorEngine;
using System.Globalization;

namespace SailorEditor;

public sealed class AggregateInspectorTemplate : DataTemplate
{
    public AggregateInspectorTemplate()
    {
        LoadTemplate = () =>
        {
            var layout = new VerticalStackLayout { Spacing = 10 };
            layout.BindingContextChanged += (_, _) => Build(layout);
            return layout;
        };
    }

    static void Build(VerticalStackLayout layout)
    {
        layout.Children.Clear();
        if (layout.BindingContext is not AggregateInspectorModel model)
        {
            return;
        }

        foreach (var section in model.Sections)
        {
            var heading = new Label { Text = section.Name, FontAttributes = FontAttributes.Bold };
            layout.Add(heading);
            if (section.Actions is { Count: > 0 } actions)
            {
                var items = actions.Select(action => new EditorContextMenuItem
                {
                    Text = action.Name,
                    Command = new AsyncRelayCommand(() => RunActionAsync(model, action))
                }).ToArray();
                FlyoutBase.SetContextFlyout(heading, MauiProgram.GetService<EditorContextMenuService>().CreateFlyout(items));
                if (section.Fields.Count == 0)
                {
                    foreach (var action in actions)
                    {
                        layout.Add(new Button
                        {
                            Text = action.Name,
                            HorizontalOptions = LayoutOptions.Start,
                            Command = new AsyncRelayCommand(() => RunActionAsync(model, action))
                        });
                    }
                }
            }

            var fields = CreateFieldsGrid();
            foreach (var field in section.Fields)
            {
                AddField(fields, field);
            }
            layout.Add(fields);
        }

        if (model.Sections.Count == 0 || model.Sections.All(section => section.Fields.Count == 0 && section.Actions is not { Count: > 0 }))
        {
            layout.Add(new Label { Text = "No common editable properties.", TextColor = Colors.Gray });
        }
    }

    static Grid CreateFieldsGrid() => new()
    {
        ColumnDefinitions = { new(GridLength.Star), new(new GridLength(2, GridUnitType.Star)) },
        ColumnSpacing = Templates.InspectorFieldSpacing,
        RowSpacing = 4,
        HorizontalOptions = LayoutOptions.Fill,
        MinimumWidthRequest = 0
    };

    static void AddField(Grid grid, InspectorAggregateField field)
    {
        if (field.Children.Count != 0 || field.Actions.Count != 0)
        {
            var heading = new Label { Text = field.Name, FontAttributes = FontAttributes.Bold };
            Templates.AddGridRow(grid, heading, GridLength.Auto);
            Grid.SetColumnSpan(heading, 2);
            if (field.IsMixed)
            {
                var marker = new Label { Text = "*", BindingContext = field };
                marker.SetBinding(Label.TextProperty, nameof(InspectorAggregateField.DisplayValue));
                Templates.AddGridRow(grid, marker, GridLength.Auto);
                Grid.SetColumnSpan(marker, 2);
            }
            var children = CreateFieldsGrid();
            children.Margin = new Thickness(8, 0, 0, 0);
            foreach (var child in field.Children)
            {
                AddField(children, child);
            }
            Templates.AddGridRow(grid, children, GridLength.Auto);
            Grid.SetColumnSpan(children, 2);
            if (field.Actions.Count != 0)
            {
                var buttons = new FlexLayout { Wrap = Microsoft.Maui.Layouts.FlexWrap.Wrap };
                foreach (var action in field.Actions)
                {
                    buttons.Add(new Button
                    {
                        Text = action.Name,
                        Padding = new Thickness(8, 2),
                        Command = new AsyncRelayCommand(() => RunActionAsync(field, action))
                    });
                }
                Templates.AddGridRow(grid, buttons, GridLength.Auto);
                Grid.SetColumnSpan(buttons, 2);
            }
            return;
        }

        Templates.AddGridRowWithLabel(grid, field.Name, CreateEditor(field), GridLength.Auto);
    }

    static View CreateEditor(InspectorAggregateField field)
    {
        if (field.IsReadOnly)
        {
            var label = new Label { BindingContext = field, LineBreakMode = LineBreakMode.WordWrap };
            label.SetBinding(Label.TextProperty, nameof(InspectorAggregateField.DisplayValue));
            return label;
        }
        if (field.ValueType == typeof(bool) || field.Choices.Length != 0)
        {
            var choices = new[] { "*" }.Concat(field.ValueType == typeof(bool) ? ["False", "True"] : field.Choices).ToList();
            var picker = Templates.EnumPicker<InspectorAggregateField>(choices,
                value => value.ChoiceValue, (value, selected) => value.SetChoice(selected));
            picker.BindingContext = field;
            return picker;
        }
        if (field.ValueType == typeof(FileId) || field.ValueType == typeof(InstanceId))
        {
            var isFile = field.ValueType == typeof(FileId);
            var editor = isFile
                ? Templates.FileIdEditor(field, nameof(InspectorAggregateField.FileIdValue),
                    (InspectorAggregateField value) => value.FileIdValue, (value, id) => value.FileIdValue = id, field.SupportedAssetType)
                : Templates.InstanceIdEditor(field, nameof(InspectorAggregateField.InstanceIdValue),
                    (InspectorAggregateField value) => value.InstanceIdValue, (value, id) => value.InstanceIdValue = id, field.ExpectedInstanceType);
            foreach (var label in Descendants(editor).OfType<Label>())
            {
                label.SetBinding(Label.TextProperty, new Binding(nameof(InspectorAggregateField.DisplayValue),
                    converter: new ReferenceDisplayConverter(isFile)));
            }
            foreach (var preview in Descendants(editor).OfType<AssetPreviewImage>())
            {
                preview.IsVisible = !field.IsMixed;
                field.PropertyChanged += (_, args) =>
                {
                    if (args.PropertyName == nameof(InspectorAggregateField.IsMixed))
                    {
                        preview.IsVisible = !field.IsMixed;
                    }
                };
            }
            field.PropertyChanged += (_, args) =>
            {
                if (args.PropertyName is nameof(InspectorAggregateField.FileIdValue) or nameof(InspectorAggregateField.InstanceIdValue) &&
                    field.HasPendingInspectorChanges && !field.HasInFlightInspectorCommit)
                {
                    _ = CommitSafelyAsync(field);
                }
            };
            return editor;
        }

        var entry = (Entry)Templates.EntryField<InspectorAggregateField, string>(value => value.DisplayValue, (_, _) => { });
        entry.BindingContext = field;
        entry.RemoveBinding(Entry.TextProperty);
        var isSynchronizingText = false;
        void SynchronizeText()
        {
            isSynchronizingText = true;
            try
            {
                entry.Text = field.DisplayValue;
            }
            finally
            {
                isSynchronizingText = false;
            }
        }
        field.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(InspectorAggregateField.DisplayValue))
            {
                SynchronizeText();
            }
        };
        SynchronizeText();
        entry.ReturnType = ReturnType.Done;
        entry.Keyboard = field.ValueType == typeof(string) ? Keyboard.Default : Keyboard.Numeric;
        entry.IsTextPredictionEnabled = field.ValueType == typeof(string);
        entry.Completed += (_, _) => entry.Unfocus();
        entry.TextChanged += (_, args) =>
        {
            if (!isSynchronizingText && entry.IsFocused)
            {
                field.SetInputText(args.NewTextValue);
            }
        };
        entry.Unfocused += async (_, _) =>
        {
            await CommitSafelyAsync(field);
            SynchronizeText();
        };
        return entry;
    }

    static IEnumerable<View> Descendants(View view)
    {
        yield return view;
        if (view is Microsoft.Maui.Controls.Layout layout)
        {
            foreach (var child in layout.Children.OfType<View>())
            {
                foreach (var descendant in Descendants(child))
                {
                    yield return descendant;
                }
            }
        }
    }

    static async Task CommitSafelyAsync(IInspectorEditable editable)
    {
        try
        {
            if (!await editable.CommitInspectorChangesAsync())
            {
                MauiProgram.GetService<EngineService>().PushConsoleMessage("The edit could not be applied to every selected target.");
            }
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"Aggregate Inspector edit failed: {error}");
        }
    }

    static async Task RunActionAsync(IInspectorEditable editable, InspectorAggregateAction action)
    {
        try
        {
            if (!await editable.CommitInspectorChangesAsync())
            {
                return;
            }
            await action.Execute(CancellationToken.None);
            MauiProgram.GetService<InspectorProjectionService>().Refresh();
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"Aggregate Inspector action '{action.Name}' failed: {error}");
            MauiProgram.GetService<EngineService>().PushConsoleMessage($"{action.Name} failed: {error.Message}");
        }
    }

    sealed class ReferenceDisplayConverter(bool isFile) : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            var text = value as string ?? string.Empty;
            if (text == "*")
            {
                return text;
            }
            return isFile
                ? new FileIdToFilenameConverter().Convert(new FileId(text), targetType, parameter, culture)
                : new InstanceIdToDisplayNameConverter().Convert(new InstanceId(text), targetType, parameter, culture);
        }
        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => BindableProperty.UnsetValue;
    }
}
