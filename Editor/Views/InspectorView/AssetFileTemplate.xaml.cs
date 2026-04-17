using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using CommunityToolkit.Mvvm.ComponentModel;

namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Compile)]
public partial class AssetFileTemplate : DataTemplate
{
    public AssetFileTemplate()
    {
        LoadTemplate = () =>
        {
            var root = new VerticalStackLayout { Spacing = 8 };
            root.BindingContextChanged += (sender, args) =>
            {
                Build((VerticalStackLayout)sender);
            };

            return root;
        };
    }

    static void Build(VerticalStackLayout layout)
    {
        layout.Children.Clear();

        if (layout.BindingContext is not AssetFile asset)
        {
            return;
        }

        asset.EnsureAssetPropertiesTyped();

        layout.Children.Add(new Views.ControlPanelView { BindingContext = asset });
        layout.Children.Add(new Label { Text = asset.AssetType?.Name ?? "Asset Metadata", FontAttributes = FontAttributes.Bold });

        var props = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = 180 },
                new ColumnDefinition { Width = GridLength.Star }
            },
            ColumnSpacing = 8,
            RowSpacing = 4
        };

        foreach (var property in asset.AssetProperties)
        {
            if (ShouldHideField(asset, property.Key))
            {
                continue;
            }

            Templates.AddGridRowWithLabel(props, property.Key, CreateFieldEditor(asset, property.Key, property.Value), GridLength.Auto);
        }

        layout.Children.Add(props);
    }

    static bool ShouldHideField(AssetFile asset, string fieldName)
    {
        return fieldName is "assetInfoType" or "fileId" or "filename";
    }

    static View CreateFieldEditor(AssetFile asset, string fieldName, ObservableObject value)
    {
        PropertyBase property = null;
        asset.AssetType?.Properties.TryGetValue(fieldName, out property);
        if (asset.ReadOnlyAssetProperties.Contains(fieldName))
        {
            return CreateReadOnlyEditor(value);
        }

        if (property is EnumProperty enumProperty && value is Observable<string> observableEnum)
        {
            var engineTypes = MauiProgram.GetService<EngineService>().EngineTypes;
            if (engineTypes.Enums.TryGetValue(enumProperty.Typename, out var values))
            {
                var picker = Templates.EnumPicker<Observable<string>>(values,
                    static (Observable<string> vm) => vm.Value,
                    static (vm, selected) => vm.Value = selected);
                picker.BindingContext = observableEnum;
                return picker;
            }
        }

        if ((property is FileIdProperty || value is Observable<FileId>) && value is Observable<FileId> observableFileId)
        {
            return Templates.FileIdEditor(observableFileId, nameof(Observable<FileId>.Value),
                static (Observable<FileId> vm) => vm.Value,
                static (vm, value) => vm.Value = value);
        }

        if (value is Observable<bool> observableBool)
        {
            var checkBox = Templates.CheckBox<Observable<bool>>(
                static (Observable<bool> vm) => vm.Value,
                static (vm, checkedValue) => vm.Value = checkedValue);
            checkBox.BindingContext = observableBool;
            return checkBox;
        }

        if (value is Observable<float> observableFloat)
        {
            var editor = Templates.FloatEditor<Observable<float>>(
                static (Observable<float> vm) => vm.Value,
                static (vm, floatValue) => vm.Value = floatValue);
            editor.BindingContext = observableFloat;
            return editor;
        }

        if (value is Observable<int> observableInt)
        {
            var intEntry = CreateEntry(Keyboard.Numeric);
            intEntry.SetBinding(Entry.TextProperty, new Binding(nameof(Observable<int>.Value), BindingMode.TwoWay));
            intEntry.BindingContext = observableInt;
            return intEntry;
        }

        if ((property is Property<List<FileId>> || value is ObservableFileIdList) && value is ObservableFileIdList fileIds)
        {
            return CreateFileIdListEditor(fileIds);
        }

        if (value is Observable<string> observableString)
        {
            var entry = CreateEntry(Keyboard.Default);
            entry.SetBinding(Entry.TextProperty, new Binding(nameof(Observable<string>.Value), BindingMode.TwoWay));
            entry.BindingContext = observableString;
            return entry;
        }

        return CreateReadOnlyEditor(value);
    }

    static Entry CreateEntry(Keyboard keyboard)
    {
        var entry = new Entry
        {
            FontSize = 12,
            Keyboard = keyboard,
            ReturnType = ReturnType.Done
        };
        entry.Completed += (sender, args) => ((Entry)sender).Unfocus();
        return entry;
    }

    static View CreateReadOnlyEditor(ObservableObject value)
    {
        var editor = new Editor
        {
            IsReadOnly = true,
            AutoSize = EditorAutoSizeOption.TextChanges,
            FontSize = 12,
            MinimumHeightRequest = 32
        };

        editor.Text = value switch
        {
            Observable<string> observableString => observableString.Value,
            Observable<FileId> observableFileId => observableFileId.Value?.Value ?? FileId.NullFileId,
            Observable<bool> observableBool => observableBool.Value.ToString().ToLowerInvariant(),
            Observable<int> observableInt => observableInt.Value.ToString(),
            Observable<float> observableFloat => observableFloat.Value.ToString(System.Globalization.CultureInfo.InvariantCulture),
            ObservableFileIdList fileIds => string.Join(Environment.NewLine, fileIds.Values.Select(fileId => fileId.Value?.Value ?? FileId.NullFileId)),
            _ => value?.ToString() ?? string.Empty
        };

        return editor;
    }

    static View CreateFileIdListEditor(ObservableFileIdList fileIds)
    {
        var listEditor = new CollectionView
        {
            ItemsSource = fileIds.Values,
            ItemTemplate = new DataTemplate(() =>
            {
                var row = new HorizontalStackLayout { Spacing = 4 };
                row.BindingContextChanged += (sender, args) =>
                {
                    row.Children.Clear();
                    if (row.BindingContext is not Observable<FileId> fileId)
                    {
                        return;
                    }

                    var removeButton = new Button { Text = "-" };
                    removeButton.Clicked += (buttonSender, clickArgs) => fileIds.Values.Remove(fileId);
                    row.Children.Add(removeButton);
                    row.Children.Add(Templates.FileIdEditor(fileId, nameof(Observable<FileId>.Value),
                        static (Observable<FileId> vm) => vm.Value,
                        static (vm, value) => vm.Value = value));
                };

                return row;
            })
        };

        var addButton = new Button { Text = "+" };
        addButton.Clicked += (sender, args) => fileIds.Values.Add(new Observable<FileId>(FileId.NullFileId));

        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += (sender, args) => fileIds.Values.Clear();

        return new VerticalStackLayout
        {
            Children =
            {
                new HorizontalStackLayout { Children = { addButton, clearButton } },
                listEditor
            }
        };
    }
}
