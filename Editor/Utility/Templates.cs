using SailorEditor.ViewModels;
using System.Globalization;
using CheckBox = Microsoft.Maui.Controls.CheckBox;
using Grid = Microsoft.Maui.Controls.Grid;
using SkiaSharp;
using Entry = Microsoft.Maui.Controls.Entry;
using System.Collections;
using DataTemplate = Microsoft.Maui.Controls.DataTemplate;
using Button = Microsoft.Maui.Controls.Button;
using System.Collections.ObjectModel;
using BindingMode = Microsoft.Maui.Controls.BindingMode;
using Binding = Microsoft.Maui.Controls.Binding;
using IValueConverter = Microsoft.Maui.Controls.IValueConverter;
using SailorEditor.Utility;
using System.ComponentModel;
using CommunityToolkit.Maui.Alerts;
using CommunityToolkit.Maui.Storage;
using System.Text;
using System.Threading;
using SailorEditor.Services;
using System.Numerics;
using CommunityToolkit.Maui.Markup;
using System.Linq.Expressions;
using Microsoft.Maui.Controls;
using YamlDotNet.Core.Tokens;
using System;
using Windows.Gaming.Input;
using System.Runtime.CompilerServices;
using System.Collections.Generic;

namespace SailorEditor.Helpers
{
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

        public static View EntryField<TBindingContext, TSource>(Expression<Func<TBindingContext, TSource>> getter, Action<TBindingContext, TSource> setter, IValueConverter valueConverter = null)
        {
            var entry = new Entry { FontSize = 12 };
            entry.Bind<Entry, TBindingContext, TSource, string>(Entry.TextProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay, converter: valueConverter);
            return entry;
        }

        public static View AssetUIDLabel<TBindingContext>(string bindingPath, Expression<Func<TBindingContext, AssetUID>> getter, Action<TBindingContext, AssetUID> setter)
        {
            var label = new Label();
            label.Behaviors.Add(new AssetUIDClickable(bindingPath));
            label.Bind<Label, TBindingContext, AssetUID, string>(Label.TextProperty,
                getter: getter,
                setter: setter,
                mode: BindingMode.Default,
                converter: new AssetUIDToFilenameConverter());

            return label;
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

        public static View CreateUniformEditor<TBindingContext, TSource, T>(
            Expression<Func<TBindingContext, ObservableList<TSource>>> getter,
            Action<TBindingContext, ObservableList<TSource>> setter,
            string labelText,
            string defaultKey = default(string),
            IValueConverter converter = null)
        where TBindingContext : ICloneable, INotifyPropertyChanged
        where TSource : Uniform<T>
        where T : IComparable<T>
        {
            Func<TBindingContext, ObservableCollection<TSource>> dictGetter = getter.Compile();

            var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

            var dictEditor = new CollectionView();
            dictEditor.ItemTemplate = new DataTemplate(() =>
                {
                    var keyEntry = new Entry();
                    keyEntry.Bind(Entry.TextProperty,
                        getter: static (Uniform<T> vm) => vm.Key,
                        setter: static (Uniform<T> vm, string value) => vm.Key = value,
                        mode: BindingMode.TwoWay);

                    IView valueView = null;

                    if (typeof(T) == typeof(AssetUID))
                    {
                        var selectButton = new Button { Text = "Select" };
                        selectButton.Clicked += async (sender, e) =>
                        {
                            var fileOpen = await FilePicker.Default.PickAsync();
                            if (fileOpen != null)
                            {
                                var AssetService = MauiProgram.GetService<AssetsService>();
                                var asset = AssetService.Files.Find((el) => el.Asset.FullName == fileOpen.FullPath);

                                if ((sender as Button).BindingContext is Uniform<AssetUID> uniform)
                                {
                                    uniform.Value = asset.UID is AssetUID uid ? uid : default(AssetUID);
                                }
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

                        image.Bind<Image, Uniform<AssetUID>, AssetUID, Image>(Image.SourceProperty, mode: BindingMode.Default, converter: new AssetUIDToTextureConverter(), getter: static (Uniform<AssetUID> vm) => vm.Value);

                        var valueEntry = new Label
                        {
                            HorizontalOptions = LayoutOptions.Center,
                            VerticalOptions = LayoutOptions.Center
                        };

                        valueEntry.Bind<Label, Uniform<AssetUID>, AssetUID, string>(Label.TextProperty, mode: BindingMode.Default, converter: converter, getter: static (Uniform<AssetUID> vm) => vm.Value);
                        valueEntry.Behaviors.Add(new AssetUIDClickable("Value"));

                        var stackLayout = new HorizontalStackLayout();
                        stackLayout.Children.Add(new HorizontalStackLayout { Children = { selectButton, image, valueEntry } });

                        valueView = stackLayout;
                    }
                    else if (typeof(T) == typeof(Vec4))
                    {
                        var valueXEntry = new Entry();
                        valueXEntry.Bind<Entry, Uniform<Vec4>, float, string>(Entry.TextProperty,
                            getter: static (Uniform<Vec4> vm) => vm.Value.X,
                            setter: static (Uniform<Vec4> vm, float value) => vm.Value.X = value,
                            mode: BindingMode.TwoWay,
                            converter: converter);

                        var valueYEntry = new Entry();
                        valueXEntry.Bind<Entry, Uniform<Vec4>, float, string>(Entry.TextProperty,
                            getter: static (Uniform<Vec4> vm) => vm.Value.Y,
                            setter: static (Uniform<Vec4> vm, float value) => vm.Value.Y = value,
                            mode: BindingMode.TwoWay,
                            converter: converter);

                        var valueZEntry = new Entry();
                        valueXEntry.Bind<Entry, Uniform<Vec4>, float, string>(Entry.TextProperty,
                            getter: static (Uniform<Vec4> vm) => vm.Value.Z,
                            setter: static (Uniform<Vec4> vm, float value) => vm.Value.Z = value,
                            mode: BindingMode.TwoWay,
                            converter: converter);

                        var valueWEntry = new Entry();
                        valueXEntry.Bind<Entry, Uniform<Vec4>, float, string>(Entry.TextProperty,
                            getter: static (Uniform<Vec4> vm) => vm.Value.W,
                            setter: static (Uniform<Vec4> vm, float value) => vm.Value.W = value,
                            mode: BindingMode.TwoWay,
                            converter: converter);

                        var stackLayout = new HorizontalStackLayout();
                        stackLayout.Children.Add(new HorizontalStackLayout { Children = { valueXEntry, valueYEntry, valueZEntry, valueWEntry } });

                        valueView = stackLayout;
                    }
                    else
                    {
                        var valueEntry = new Entry();
                        valueEntry.Bind<Entry, Uniform<T>, T, string>(Entry.TextProperty,
                           getter: static (Uniform<T> vm) => vm.Value,
                           setter: static (Uniform<T> vm, T value) => vm.Value = value,
                           mode: BindingMode.TwoWay,
                           converter: converter);

                        valueView = valueEntry;
                    }

                    var deleteButton = new Button { Text = "-" };
                    deleteButton.Clicked += (sender, e) =>
                    {
                        var dict = dictGetter((TBindingContext)dictEditor.BindingContext);

                        if ((sender as Button).BindingContext is TSource value)
                        {
                            dict.Remove(value);
                        }
                    };

                    var entryLayout = new HorizontalStackLayout { Children = { deleteButton, valueView, keyEntry } };

                    return entryLayout;
                });

            dictEditor.Bind(ItemsView.ItemsSourceProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay);

            var addButton = new Button { Text = "+" };
            addButton.Clicked += (sender, e) =>
            {
                var dict = dictGetter((TBindingContext)dictEditor.BindingContext);
                if (dict != null)
                {
                    TSource v = (TSource)new Uniform<T> { Key = defaultKey, Value = default(T) };
                    dict.Add(v);
                }
            };

            var clearButton = new Button { Text = "Clear" };
            clearButton.Clicked += (sender, e) =>
            {
                var dict = dictGetter((TBindingContext)dictEditor.BindingContext);
                if (dict != null)
                {
                    dict.Clear();
                }
            };

            var stackLayout = new VerticalStackLayout();
            stackLayout.Children.Add(label);
            stackLayout.Children.Add(new HorizontalStackLayout { Children = { addButton, clearButton } });
            stackLayout.Children.Add(dictEditor);

            return stackLayout;
        }

        public static View CreateListEditor<TBindingContext, TSource>(Expression<Func<TBindingContext, ObservableList<TSource>>> getter, Action<TBindingContext, ObservableList<TSource>> setter, string labelText, TSource defaultElement = default(TSource), IValueConverter converter = null)
        where TBindingContext : ICloneable, INotifyPropertyChanged
        where TSource : INotifyPropertyChanged, ICloneable
        {
            var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

            Func<TBindingContext, ObservableCollection<TSource>> listGetter = getter.Compile();

            var listEditor = new CollectionView();
            listEditor.ItemTemplate = new DataTemplate(() =>
                {
                    var deleteButton = new Button { Text = "-" };
                    deleteButton.Clicked += (sender, e) =>
                    {
                        var list = listGetter((TBindingContext)listEditor.BindingContext);
                        if ((sender as Button).BindingContext is TSource value)
                        {
                            list.Remove(value);
                        }
                    };

                    var entryLayout = new HorizontalStackLayout();
                    entryLayout.Children.Add(deleteButton);

                    if (typeof(TSource) == typeof(Observable<AssetUID>))
                    {
                        var selectButton = new Button { Text = "Select" };
                        selectButton.Clicked += async (sender, e) =>
                        {
                            var fileOpen = await FilePicker.Default.PickAsync();
                            if (fileOpen != null)
                            {
                                var AssetService = MauiProgram.GetService<AssetsService>();
                                var asset = AssetService.Files.Find((el) => el.Asset.FullName == fileOpen.FullPath);

                                if ((sender as Button).BindingContext is Observable<AssetUID> value)
                                {
                                    value.Value = asset.UID is AssetUID uid ? uid : default(AssetUID);
                                }
                            }
                        };

                        entryLayout.Children.Add(selectButton);
                    }

                    if (typeof(TSource) == typeof(Observable<AssetUID>))
                    {
                        entryLayout.Children.Add(AssetUIDLabel("Value",
                            static (Observable<AssetUID> vm) => vm.Value,
                            static (Observable<AssetUID> vm, AssetUID value) => vm.Value = value));
                    }
                    else if (typeof(TSource) == typeof(Observable<string>))
                    {
                        entryLayout.Children.Add(EntryField(
                            static (Observable<string> vm) => vm.Value,
                            static (Observable<string> vm, string value) => vm.Value = value));
                    }

                    return entryLayout;
                });

            listEditor.Bind(ItemsView.ItemsSourceProperty, getter: getter, setter: setter, mode: BindingMode.TwoWay);

            var addButton = new Button { Text = "+" };
            addButton.Clicked += (sender, e) =>
            {
                var list = listGetter((TBindingContext)listEditor.BindingContext);
                if (list != null)
                {
                    list.Add((TSource)defaultElement.Clone());
                }
            };

            var clearButton = new Button { Text = "Clear" };
            clearButton.Clicked += (sender, e) =>
            {
                var list = listGetter((TBindingContext)listEditor.BindingContext);
                if (list != null)
                {
                    list.Clear();
                }
            };

            var stackLayout = new VerticalStackLayout();
            stackLayout.Children.Add(label);
            stackLayout.Children.Add(new HorizontalStackLayout { Children = { addButton, clearButton } });
            stackLayout.Children.Add(listEditor);

            return stackLayout;
        }

        public static ImageSource ResizeImageToThumbnail(string imagePath)
        {
            using (var original = SKBitmap.Decode(imagePath))
            {
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

                using (var resizedImage = original.Resize(new SKImageInfo(width, height), SKFilterQuality.High))
                {
                    if (resizedImage == null) return null;

                    using (var image = SKImage.FromBitmap(resizedImage))
                    using (var data = image.Encode(SKEncodedImageFormat.Png, 100))
                    {
                        var stream = new MemoryStream();
                        data.SaveTo(stream);
                        stream.Seek(0, SeekOrigin.Begin);

                        return ImageSource.FromStream(() => stream);
                    }
                }
            }
        }
    };
}