using SailorEditor.ViewModels;
using CheckBox = Microsoft.Maui.Controls.CheckBox;
using Grid = Microsoft.Maui.Controls.Grid;
using SkiaSharp;
using Entry = Microsoft.Maui.Controls.Entry;
using DataTemplate = Microsoft.Maui.Controls.DataTemplate;
using Button = Microsoft.Maui.Controls.Button;
using System.Collections.ObjectModel;
using BindingMode = Microsoft.Maui.Controls.BindingMode;
using IValueConverter = Microsoft.Maui.Controls.IValueConverter;
using SailorEditor.Utility;
using System.ComponentModel;
using SailorEditor.Services;
using CommunityToolkit.Maui.Markup;
using System.Linq.Expressions;
using SailorEngine;
using Component = SailorEditor.ViewModels.Component;
using SailorEditor.Views;
using SailorEditor.Controls;
using System;

namespace SailorEditor.Helpers;
static class Templates
{
    public const int ThumbnailSize = 128;

    public static Editor ReadOnlyTextView<T>(Expression<Func<T, string>> prop)
    {
        var editor = new Editor
        {
            FontSize = 12,
            IsReadOnly = true,
            AutoSize = EditorAutoSizeOption.TextChanges
        };

        editor.Bind(Editor.TextProperty, getter: prop);
        return editor;
    }

    public static View ShaderCodeView(string title, Expression<Func<ShaderFile, string>> prop)
    {
        var titleLabel = new Label
        {
            Text = title,
            VerticalOptions = LayoutOptions.Center
        };

        var codeView = ReadOnlyTextView(prop);

        var stackLayout = new VerticalStackLayout();
        stackLayout.Children.Add(titleLabel);
        stackLayout.Children.Add(codeView);

        return stackLayout;
    }

    public static CheckBox CheckBox<T>(Expression<Func<T, bool>> getter, Action<T, bool> setter)
    {
        var checkBox = new Microsoft.Maui.Controls.CheckBox();
        checkBox.Bind(Microsoft.Maui.Controls.CheckBox.IsCheckedProperty, getter: getter, setter: setter);
        return checkBox;
    }

    public static Picker EnumPicker<TBinding, TEnum>(Expression<Func<TBinding, TEnum>> getter, Action<TBinding, TEnum> setter) where TEnum : struct
    {
        var picker = new Picker
        {
            FontSize = 12
        };

        var enumValues = Enum.GetValues(typeof(TEnum)).Cast<TEnum>();
        foreach (var value in enumValues)
        {
            picker.Items.Add(value.ToString());
        }

        picker.Bind<Picker, TBinding, TEnum, string>(Picker.SelectedItemProperty, getter: getter, setter: setter, BindingMode.TwoWay, new EnumToStringConverter<TEnum>());
        return picker;
    }

    public static Picker EnumPicker<TBinding>(List<string> enumValues, Expression<Func<TBinding, string>> getter, Action<TBinding, string> setter)
    {
        var picker = new Picker
        {
            FontSize = 12
        };

        foreach (var value in enumValues)
        {
            picker.Items.Add(value.ToString());
        }

        picker.Bind(Picker.SelectedItemProperty, getter: getter, setter: setter, BindingMode.TwoWay);
        return picker;
    }

    public static View EntryField<TBindingContext, TSource>(Expression<Func<TBindingContext, TSource>> getter, Action<TBindingContext, TSource> setter, IValueConverter valueConverter = null)
    {
        var entry = new Entry { FontSize = 12 };
        entry.Bind<Entry, TBindingContext, TSource, string>(Entry.TextProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay, converter: valueConverter);
        return entry;
    }

    public static View FileIdLabel<TBindingContext>(string bindingPath, Expression<Func<TBindingContext, FileId>> getter, Action<TBindingContext, FileId> setter)
    {
        var label = new Label();
        label.Behaviors.Add(new FileIdSelectBehavior());
        label.Bind<Label, TBindingContext, FileId, string>(Label.TextProperty,
            getter: getter,
            setter: setter,
            mode: BindingMode.Default,
            converter: new FileIdToFilenameConverter());

        return label;
    }

    public static View TextureEditor<TBindingContext>(Expression<Func<TBindingContext, FileId>> getter, Action<TBindingContext, FileId> setter)
    {
        var selectButton = new Button { Text = "Select" };
        selectButton.Clicked += async (sender, e) =>
        {
            var fileOpen = await FilePicker.Default.PickAsync();
            if (fileOpen != null)
            {
                var AssetService = MauiProgram.GetService<AssetsService>();
                var asset = AssetService.Files.Find((el) => el.Asset.FullName == fileOpen.FullPath);
                setter((TBindingContext)(sender as Button).BindingContext, asset.FileId is FileId id ? id : default);
            }
        };

        var image = new Image
        {
            WidthRequest = 64,
            HeightRequest = 64,
            Aspect = Aspect.AspectFit,
            HorizontalOptions = LayoutOptions.Start,
            VerticalOptions = LayoutOptions.Start
        };

        image.Bind<Image, Uniform<FileId>, FileId, Image>(Image.SourceProperty,
            mode: BindingMode.Default,
            converter: new FileIdToPreviewTextureConverter(),
            getter: static (Uniform<FileId> vm) => vm.Value);

        var valueEntry = new Label
        {
            HorizontalOptions = LayoutOptions.Center,
            VerticalOptions = LayoutOptions.Center
        };

        valueEntry.Bind<Label, Uniform<FileId>, FileId, string>(Label.TextProperty,
            mode: BindingMode.Default,
            converter: new FileIdToFilenameConverter(),
            getter: static (Uniform<FileId> vm) => vm.Value);

        valueEntry.Behaviors.Add(new FileIdSelectBehavior());

        var stackLayout = new HorizontalStackLayout();
        stackLayout.Children.Add(new HorizontalStackLayout { Children = { selectButton, image, valueEntry } });

        return stackLayout;
    }

    public static View FileIdEditor<TBindingContext>(object bindingContext, string bindingPath, Expression<Func<TBindingContext, FileId>> getter, Action<TBindingContext, FileId> setter)
    {
        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += async (sender, e) => setter((TBindingContext)bindingContext, new FileId());

        var valueEntry = new Label
        {
            HorizontalOptions = LayoutOptions.Center,
            VerticalOptions = LayoutOptions.Center
        };

        valueEntry.BindingContext = bindingContext;

        valueEntry.Bind<Label, TBindingContext, FileId, string>(Label.TextProperty,
            mode: BindingMode.TwoWay,
            converter: new FileIdToFilenameConverter(),
            getter: getter,
            setter: setter);

        var dragAndDropBehaviour = new FileIdDragAndDropBehaviour();
        dragAndDropBehaviour.SetBinding(FileIdDragAndDropBehaviour.BoundPropertyProperty, new Binding(bindingPath));

        var selectBehavior = new FileIdSelectBehavior();
        selectBehavior.SetBinding(FileIdSelectBehavior.BoundPropertyProperty, new Binding(bindingPath));

        valueEntry.Behaviors.Add(dragAndDropBehaviour);
        valueEntry.Behaviors.Add(selectBehavior);

        var stackLayout = new HorizontalStackLayout();
        stackLayout.Children.Add(new HorizontalStackLayout { Children = { clearButton, valueEntry } });

        return stackLayout;
    }

    public static View InstanceIdEditor<TBindingContext>(object bindingContext, string bindingPath, Expression<Func<TBindingContext, InstanceId>> getter, Action<TBindingContext, InstanceId> setter)
    {
        var valueEntry = new Label
        {
            HorizontalOptions = LayoutOptions.Center,
            VerticalOptions = LayoutOptions.Center
        };

        valueEntry.BindingContext = bindingContext;

        valueEntry.Bind<Label, TBindingContext, InstanceId, string>(Label.TextProperty,
            mode: BindingMode.TwoWay,
            converter: new InstanceIdToDisplayNameConverter(),
            getter: getter,
            setter: setter);

        var selectBehavior = new InstanceIdSelectBehavior();
        selectBehavior.SetBinding(InstanceIdSelectBehavior.BoundPropertyProperty, new Binding(bindingPath));

        valueEntry.Behaviors.Add(selectBehavior);

        var stackLayout = new HorizontalStackLayout();
        stackLayout.Children.Add(new HorizontalStackLayout { Children = { valueEntry } });

        return stackLayout;
    }

    public static View Vec2Editor<TBindingContext>(Func<TBindingContext, Vec2> convert)
    {
        var stackLayout = new HorizontalStackLayout();
        stackLayout.Children.Add(new HorizontalStackLayout
        {
            Children =
            {
                FloatEditor((TBindingContext vm) => convert(vm).X, (TBindingContext vm, float value) => convert(vm).X = value),
                FloatEditor((TBindingContext vm) => convert(vm).Y, (TBindingContext vm, float value) => convert(vm).Y = value)
            }
        });

        return stackLayout;
    }

    public static View Vec3Editor<TBindingContext>(Func<TBindingContext, Vec3> convert)
    {
        var stackLayout = new HorizontalStackLayout();
        stackLayout.Children.Add(new HorizontalStackLayout
        {
            Children =
            {
                FloatEditor((TBindingContext vm) => convert(vm).X, (TBindingContext vm, float value) => convert(vm).X = value),
                FloatEditor((TBindingContext vm) => convert(vm).Y, (TBindingContext vm, float value) => convert(vm).Y = value),
                FloatEditor((TBindingContext vm) => convert(vm).Z, (TBindingContext vm, float value) => convert(vm).Z = value)
            }
        });

        return stackLayout;
    }

    public static View RotationEditor<TBindingContext>(Func<TBindingContext, Rotation> convert)
    {
        var stackLayout = new HorizontalStackLayout();
        stackLayout.Children.Add(new HorizontalStackLayout
        {
            Children =
            {
                FloatEditor((TBindingContext vm) => convert(vm).Yaw, (TBindingContext vm, float value) => convert(vm).Yaw = value),
                FloatEditor((TBindingContext vm) => convert(vm).Pitch, (TBindingContext vm, float value) => convert(vm).Pitch = value),
                FloatEditor((TBindingContext vm) => convert(vm).Roll, (TBindingContext vm, float value) => convert(vm).Roll = value)
            }
        });

        return stackLayout;
    }

    public static View Vec4Editor<TBindingContext>(Func<TBindingContext, Vec4> convert)
    {
        var stackLayout = new HorizontalStackLayout();
        stackLayout.Children.Add(new HorizontalStackLayout
        {
            Children =
            {
                FloatEditor((TBindingContext vm) => convert(vm).X, (TBindingContext vm, float value) => convert(vm).X = value),
                FloatEditor((TBindingContext vm) => convert(vm).Y, (TBindingContext vm, float value) => convert(vm).Y = value),
                FloatEditor((TBindingContext vm) => convert(vm).Z, (TBindingContext vm, float value) => convert(vm).Z = value),
                FloatEditor((TBindingContext vm) => convert(vm).W, (TBindingContext vm, float value) => convert(vm).W = value)
            }
        });

        return stackLayout;
    }

    public static View FloatEditor<TBindingContext>(Expression<Func<TBindingContext, float>> getter, Action<TBindingContext, float> setter)
    {
        var value = new Entry
        {
            Keyboard = Keyboard.Numeric,
            ReturnType = ReturnType.Done,
            IsTextPredictionEnabled = false,
        };

        value.Completed += (sender, args) => { ((Entry)sender).Unfocus(); };

        value.Bind<Entry, TBindingContext, float, string>(Entry.TextProperty,
            getter: getter,
            setter: setter,
            mode: BindingMode.TwoWay,
            converter: new FloatValueConverter());

        return value;
    }

    public static View UniformEditor<TBindingContext, T>(
        Expression<Func<TBindingContext, ObservableList<Uniform<T>>>> getter,
        Action<TBindingContext, ObservableList<Uniform<T>>> setter,
        Func<IView> valueEditor,
        string labelText,
        string defaultKey = default,
        T defaultValue = default)
    where TBindingContext : ICloneable, INotifyPropertyChanged
    where T : IComparable<T>
    {
        Func<TBindingContext, ObservableCollection<Uniform<T>>> dictGetter = getter.Compile();

        var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

        var dictEditor = new CollectionView();
        dictEditor.ItemTemplate = new DataTemplate(() =>
            {
                var keyEntry = new Entry();
                keyEntry.Bind(Entry.TextProperty,
                    getter: static (Uniform<T> vm) => vm.Key,
                    setter: static (Uniform<T> vm, string value) => vm.Key = value,
                    mode: BindingMode.TwoWay);

                var deleteButton = new Button { Text = "-" };
                deleteButton.Clicked += (sender, e) =>
                {
                    var dict = dictGetter((TBindingContext)dictEditor.BindingContext);

                    if ((sender as Button).BindingContext is Uniform<T> value)
                    {
                        dict.Remove(value);
                    }
                };

                var entryLayout = new HorizontalStackLayout { Children = { deleteButton, valueEditor(), keyEntry } };

                return entryLayout;
            });

        dictEditor.Bind(ItemsView.ItemsSourceProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay);

        var addButton = new Button { Text = "+" };
        addButton.Clicked += (sender, e) =>
        {
            var dict = dictGetter((TBindingContext)dictEditor.BindingContext);
            if (dict != null)
            {
                Uniform<T> v = new() { Key = defaultKey, Value = defaultValue };

                if (defaultValue is ICloneable cloneable)
                    v.Value = (T)cloneable.Clone();

                dict.Add(v);
            }
        };

        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += (sender, e) =>
        {
            var dict = dictGetter((TBindingContext)dictEditor.BindingContext);
            dict?.Clear();
        };

        var stackLayout = new VerticalStackLayout();
        stackLayout.Children.Add(label);
        stackLayout.Children.Add(new HorizontalStackLayout { Children = { addButton, clearButton } });
        stackLayout.Children.Add(dictEditor);

        return stackLayout;
    }

    public static View ListEditor<TBindingContext, T>(
        Expression<Func<TBindingContext, ObservableList<Observable<T>>>> getter,
        Action<TBindingContext, ObservableList<Observable<T>>> setter,
        Func<IView> valueEditor,
        string labelText,
        T defaultElement = default,
        IValueConverter converter = null)
    where TBindingContext : ICloneable, INotifyPropertyChanged
    where T : ICloneable, IComparable<T>
    {
        var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

        Func<TBindingContext, ObservableCollection<Observable<T>>> listGetter = getter.Compile();

        var listEditor = new CollectionView();
        listEditor.ItemTemplate = new DataTemplate(() =>
            {
                var deleteButton = new Button { Text = "-" };
                deleteButton.Clicked += (sender, e) =>
                {
                    var list = listGetter((TBindingContext)listEditor.BindingContext);
                    if ((sender as Button).BindingContext is Observable<T> value)
                    {
                        list.Remove(value);
                    }
                };

                var entryLayout = new HorizontalStackLayout { Children = { deleteButton, valueEditor() } };
                return entryLayout;
            });

        listEditor.Bind(ItemsView.ItemsSourceProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay);

        var addButton = new Button { Text = "+" };
        addButton.Clicked += (sender, e) =>
        {
            var list = listGetter((TBindingContext)listEditor.BindingContext);
            list?.Add(new Observable<T>(defaultElement));
        };

        var clearButton = new Button { Text = "Clear" };
        clearButton.Clicked += (sender, e) =>
        {
            var list = listGetter((TBindingContext)listEditor.BindingContext);
            list?.Clear();
        };

        var stackLayout = new VerticalStackLayout();
        stackLayout.Children.Add(label);
        stackLayout.Children.Add(new HorizontalStackLayout { Children = { addButton, clearButton } });
        stackLayout.Children.Add(listEditor);

        return stackLayout;
    }

    public static ImageSource ResizeImageToThumbnail(string imagePath)
    {
        using var original = SKBitmap.Decode(imagePath);
        if (original == null)
        {
            return null;
        }

        int width = ThumbnailSize;
        int height = ThumbnailSize;

        float aspectRatio = original.Width / (float)original.Height;
        if (original.Width > original.Height)
        {
            height = (int)(width / aspectRatio);
        }
        else
        {
            width = (int)(height * aspectRatio);
        }

        using var resizedImage = original.Resize(new SKImageInfo(width, height), SKFilterQuality.High);

        if (resizedImage == null) return null;

        using var image = SKImage.FromBitmap(resizedImage);
        using var data = image.Encode(SKEncodedImageFormat.Png, 100);

        var stream = new MemoryStream();
        data.SaveTo(stream);
        stream.Seek(0, SeekOrigin.Begin);

        return ImageSource.FromStream(() => stream);
    }

    public static void AddGridRow(Microsoft.Maui.Controls.Grid grid, View view, Microsoft.Maui.GridLength gridLength)
    {
        grid.RowDefinitions.Add(new Microsoft.Maui.Controls.RowDefinition { Height = gridLength });
        grid.Children.Add(view);
        Grid.SetRow(view, grid.RowDefinitions.Count - 1);
    }

    public static void AddGridRowWithLabel(Grid grid, string labelText, View contentView, Microsoft.Maui.GridLength gridLength)
    {
        grid.RowDefinitions.Add(new Microsoft.Maui.Controls.RowDefinition { Height = gridLength });

        var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, HorizontalOptions = LayoutOptions.Start };

        grid.Add(label, 0, grid.RowDefinitions.Count - 1);
        grid.Add(contentView, 1, grid.RowDefinitions.Count - 1);
    }
};