using SailorEditor.Controls;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Globalization;

namespace SailorEditor.Helpers;

static partial class Templates
{
    public static View FloatEditor(InspectorAggregateField property)
    {
        var editor = property.Range is { } range
            ? RangedFloatEditor<InspectorAggregateField>(value => value.FloatValue, (value, number) => ApplyAggregateRangeValue(value, number), range)
            : FloatEditor<InspectorAggregateField>(value => value.FloatValue, (value, number) => value.SetDraft(number));
        return ConfigureAggregateEntries(editor, property);
    }

    public static View IntEditor(InspectorAggregateField property)
    {
        var editor = property.Range is { } range
            ? RangedIntEditor<InspectorAggregateField>(value => value.IntValue, (value, number) => ApplyAggregateRangeValue(value, number), range)
            : IntEditor<InspectorAggregateField>(value => value.IntValue, (value, number) => value.SetDraft(number));
        return ConfigureAggregateEntries(editor, property);
    }

    public static View UIntEditor(InspectorAggregateField property)
    {
        var editor = property.Range is { } range
            ? RangedUIntEditor<InspectorAggregateField>(value => value.UIntValue, (value, number) => ApplyAggregateRangeValue(value, number), range)
            : UIntEditor<InspectorAggregateField>(value => value.UIntValue, (value, number) => value.SetDraft(number));
        return ConfigureAggregateEntries(editor, property);
    }

    public static View StringEditor(InspectorAggregateField property)
        => ConfigureAggregateEntries(StringEditor<InspectorAggregateField>(value => value.DisplayValue, (_, _) => { }), property);

    public static View BoolEditor(InspectorAggregateField property)
    {
        var checkBox = (Microsoft.Maui.Controls.CheckBox)BoolEditor<InspectorAggregateField>(
            value => (bool)(value.Value ?? false), (_, _) => { });
        checkBox.RemoveBinding(Microsoft.Maui.Controls.CheckBox.IsCheckedProperty);
        checkBox.BindingContext = property;
        var marker = new Label { Text = "*", VerticalOptions = LayoutOptions.Center };
        var synchronizing = false;
        void Synchronize()
        {
            synchronizing = true;
            try
            {
                checkBox.IsChecked = property.Value is true;
                marker.IsVisible = property.IsMixed;
            }
            finally
            {
                synchronizing = false;
            }
        }
        property.PropertyChanged += (_, _) => Synchronize();
        checkBox.CheckedChanged += (_, args) =>
        {
            if (!synchronizing)
            {
                property.SetDraft(args.Value);
                CommitInspectorBindingContext(property);
            }
        };
        Synchronize();
        return new HorizontalStackLayout { Spacing = InspectorFieldSpacing, Children = { checkBox, marker } };
    }

    public static View EnumPicker(InspectorAggregateField property)
    {
        var choices = property.IsMixed ? new[] { "*" }.Concat(property.Choices).ToList() : property.Choices.ToList();
        var picker = EnumPicker<InspectorAggregateField>(choices, value => value.ChoiceValue,
            (value, choice) => value.SetChoice(choice), observeContext: true);
        picker.BindingContext = property;
        return picker;
    }

    public static View Vec2Editor(InspectorAggregateField property) => CreateAggregateVectorEditor(property);
    public static View Vec3Editor(InspectorAggregateField property) => CreateAggregateVectorEditor(property);
    public static View Vec4Editor(InspectorAggregateField property) => CreateAggregateVectorEditor(property);
    public static View RotationEditor(InspectorAggregateField property) => CreateAggregateVectorEditor(property);

    static View CreateAggregateVectorEditor(InspectorAggregateField property)
        => CreateInlineFieldGrid(property.Children.Select(FloatEditor).ToArray());

    public static View FileIdEditor(InspectorAggregateField property)
    {
        var editor = FileIdEditor(property, nameof(InspectorAggregateField.FileIdValue),
            (InspectorAggregateField value) => value.FileIdValue, (value, id) => value.FileIdValue = id, property.SupportedAssetType);
        return ConfigureAggregateReference(editor, property, isFile: true);
    }

    public static View InstanceIdEditor(InspectorAggregateField property)
    {
        var editor = InstanceIdEditor(property, nameof(InspectorAggregateField.InstanceIdValue),
            (InspectorAggregateField value) => value.InstanceIdValue, (value, id) => value.InstanceIdValue = id, property.ExpectedInstanceType);
        return ConfigureAggregateReference(editor, property, isFile: false);
    }

    public static View ListEditor(InspectorAggregateField property, Func<InspectorAggregateField, View> createEditor,
        Func<InspectorAggregateAction, Task> runAction)
    {
        var rows = new VerticalStackLayout { Spacing = InspectorFieldSpacing, HorizontalOptions = LayoutOptions.Fill };
        for (var index = 0; index < property.Children.Count; ++index)
        {
            var editor = createEditor(property.Children[index]);
            var suffix = $"[{index + 1}]";
            AttachAggregateActions(editor, property.Actions.Where(action => action.Name.EndsWith(suffix, StringComparison.Ordinal)), runAction,
                action => action.Name[..^suffix.Length].TrimEnd());
            rows.Children.Add(editor);
        }
        AttachAggregateActions(rows, property.Actions.Where(action => action.Name is "Add" or "Clear"), runAction);
        return rows;
    }

    public static void AttachAggregateActions(BindableObject target, IEnumerable<InspectorAggregateAction> actions,
        Func<InspectorAggregateAction, Task> runAction, Func<InspectorAggregateAction, string>? displayName = null)
    {
        var items = actions.Select(action => new EditorContextMenuItem
        {
            Text = displayName?.Invoke(action) ?? action.Name,
            Command = new Command(() => RunAggregateActionSafely(runAction, action))
        }).ToArray();
        if (items.Length != 0)
        {
            FlyoutBase.SetContextFlyout(target, MauiProgram.GetService<EditorContextMenuService>().CreateFlyout(items));
        }
    }

    static async void RunAggregateActionSafely(Func<InspectorAggregateAction, Task> runAction, InspectorAggregateAction action)
    {
        try
        {
            await runAction(action);
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"Inspector action '{action.Name}' failed: {error}");
            MauiProgram.GetService<EngineService>().PushConsoleMessage($"{action.Name} failed: {error.Message}");
        }
    }

    static void ApplyAggregateRangeValue(InspectorAggregateField property, object value)
    {
        property.SetDraft(value);
        CommitInspectorBindingContext(property);
    }

    static View ConfigureAggregateEntries(View editor, InspectorAggregateField property)
    {
        foreach (var entry in InspectorDescendants(editor).OfType<Entry>())
        {
            entry.RemoveBinding(Entry.TextProperty);
            entry.BindingContext = property;
            var synchronizing = false;
            void Synchronize()
            {
                synchronizing = true;
                try
                {
                    entry.Text = property.DisplayValue;
                }
                finally
                {
                    synchronizing = false;
                }
            }
            property.PropertyChanged += (_, args) =>
            {
                if (args.PropertyName == nameof(InspectorAggregateField.DisplayValue))
                {
                    Synchronize();
                }
            };
            entry.TextChanged += (_, args) =>
            {
                if (!synchronizing && entry.IsFocused)
                {
                    property.SetInputText(args.NewTextValue);
                }
            };
            entry.Unfocused += (_, _) =>
            {
                CommitInspectorBindingContext(property);
                Synchronize();
            };
            Synchronize();
        }
        editor.BindingContext = property;
        return editor;
    }

    static View ConfigureAggregateReference(View editor, InspectorAggregateField property, bool isFile)
    {
        foreach (var label in InspectorDescendants(editor).OfType<Label>())
        {
            label.SetBinding(Label.TextProperty, new Binding(nameof(InspectorAggregateField.DisplayValue),
                converter: new AggregateReferenceDisplayConverter(isFile)));
        }
        foreach (var preview in InspectorDescendants(editor).OfType<AssetPreviewImage>())
        {
            preview.IsVisible = !property.IsMixed;
            property.PropertyChanged += (_, _) => preview.IsVisible = !property.IsMixed;
        }
        property.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(InspectorAggregateField.FileIdValue) or nameof(InspectorAggregateField.InstanceIdValue) &&
                property.HasPendingInspectorChanges && !property.HasInFlightInspectorCommit)
            {
                CommitInspectorBindingContext(property);
            }
        };
        return editor;
    }

    static IEnumerable<View> InspectorDescendants(View view)
    {
        yield return view;
        if (view is Microsoft.Maui.Controls.Layout layout)
        {
            foreach (var child in layout.Children.OfType<View>())
            {
                foreach (var descendant in InspectorDescendants(child))
                {
                    yield return descendant;
                }
            }
        }
    }

    sealed class AggregateReferenceDisplayConverter(bool isFile) : IValueConverter
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
