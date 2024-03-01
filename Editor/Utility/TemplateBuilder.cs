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

namespace SailorEditor.Helpers
{
    using AssetUID = string;
    static class TemplateBuilder
    {
        public const int ThumbnailSize = 128;

        public static Editor CreateReadOnlyEditor(string bindingPath)
        {
            var editor = new Editor
            {
                FontSize = 12,
                IsReadOnly = true,
                AutoSize = EditorAutoSizeOption.TextChanges
            };

            editor.SetBinding(Editor.TextProperty, new Binding(bindingPath));
            return editor;
        }

        public static View CreateShaderCodeView(string title, string bindingPath)
        {
            var titleLabel = new Label
            {
                Text = title,
                VerticalOptions = LayoutOptions.Center
            };

            var codeView = CreateReadOnlyEditor(bindingPath);

            var stackLayout = new VerticalStackLayout();
            stackLayout.Children.Add(titleLabel);
            stackLayout.Children.Add(codeView);

            return stackLayout;
        }

        public static CheckBox CreateCheckBox(string bindingPath)
        {
            var checkBox = new Microsoft.Maui.Controls.CheckBox();
            checkBox.SetBinding(CheckBox.IsCheckedProperty, new Binding(bindingPath));
            return checkBox;
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
        public static Picker CreateEnumPicker<TEnum>(string bindingPath) where TEnum : struct
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

            picker.SetBinding(Picker.SelectedItemProperty, new Binding(bindingPath, BindingMode.TwoWay, new EnumToStringConverter<TEnum>(), null));

            return picker;
        }
        public static View CreateEntry(string bindingPath)
        {
            var entry = new Entry
            {
                FontSize = 12
            };
            entry.SetBinding(Entry.TextProperty, new Binding(bindingPath, BindingMode.TwoWay));

            return entry;
        }
        public static View CreateUniformEditor<T>(string bindingPath, string labelText, string defaultKey = default(string), IValueConverter converter = null)
        where T : IComparable<T>
        {
            var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

            var dictionaryEditor = new CollectionView();
            dictionaryEditor.ItemTemplate = new DataTemplate(() =>
                {
                    var keyEntry = new Entry();
                    keyEntry.SetBinding(Entry.TextProperty, "Key", BindingMode.TwoWay);

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

                                if ((sender as Button).BindingContext is Uniform<T> uniform)
                                {
                                    if (asset.UID is T uid)
                                        uniform.Value = uid;
                                    else
                                        uniform.Value = default(T);
                                }
                            }
                        };

                        var valueEntry = new Label();
                        valueEntry.SetBinding(Label.TextProperty, new Binding("Value", BindingMode.Default, converter));
                        valueEntry.Behaviors.Add(new AssetUIDClickable("Value"));

                        var stackLayout = new HorizontalStackLayout();
                        stackLayout.Children.Add(new HorizontalStackLayout { Children = { selectButton, valueEntry } });

                        valueView = stackLayout;
                    }
                    if (typeof(T) == typeof(Vec4))
                    {
                        var valueXEntry = new Entry();
                        valueXEntry.SetBinding(Entry.TextProperty, new Binding("Value.X", BindingMode.TwoWay, converter));

                        var valueYEntry = new Entry();
                        valueYEntry.SetBinding(Entry.TextProperty, new Binding("Value.Y", BindingMode.TwoWay, converter));

                        var valueZEntry = new Entry();
                        valueZEntry.SetBinding(Entry.TextProperty, new Binding("Value.Z", BindingMode.TwoWay, converter));

                        var valueWEntry = new Entry();
                        valueWEntry.SetBinding(Entry.TextProperty, new Binding("Value.W", BindingMode.TwoWay, converter));

                        var stackLayout = new HorizontalStackLayout();
                        stackLayout.Children.Add(new HorizontalStackLayout { Children = { valueXEntry, valueYEntry, valueZEntry, valueWEntry } });

                        valueView = stackLayout;
                    }
                    else
                    {
                        var valueEntry = new Entry();
                        valueEntry.SetBinding(Entry.TextProperty, "Value", BindingMode.TwoWay, converter);

                        valueView = valueEntry;
                    }

                    var deleteButton = new Button { Text = "-" };
                    deleteButton.Clicked += (sender, e) =>
                    {
                        var dictionaryProperty = (dictionaryEditor.BindingContext).GetType().GetProperty(bindingPath);
                        if (dictionaryProperty.GetValue(dictionaryEditor.BindingContext) is IList<Uniform<T>> dict)
                        {
                            if ((sender as Button).BindingContext is Uniform<T> uniform)
                            {
                                dict.Remove(uniform);
                            }
                        }
                    };

                    var entryLayout = new HorizontalStackLayout { Children = { keyEntry, valueView, deleteButton } };

                    return entryLayout;
                });

            dictionaryEditor.SetBinding(ItemsView.ItemsSourceProperty, new Binding(bindingPath, BindingMode.TwoWay));

            var addButton = new Button { Text = "+" };
            addButton.Clicked += (sender, e) =>
            {
                var property = (sender as Button).BindingContext.GetType().GetProperty(bindingPath);
                if (property != null)
                {
                    if (property.GetValue((sender as Button).BindingContext) is IList dict)
                    {
                        dict.Add(new Uniform<T> { Key = defaultKey, Value = default(T) });
                    }
                }
            };

            var clearButton = new Button { Text = "Clear" };
            clearButton.Clicked += (sender, e) =>
            {
                var property = (sender as Button).BindingContext.GetType().GetProperty(bindingPath);
                if (property != null)
                {
                    if (property.GetValue((sender as Button).BindingContext) is IList list)
                    {
                        list.Clear();
                    }
                }
            };

            var stackLayout = new VerticalStackLayout();
            stackLayout.Children.Add(label);
            stackLayout.Children.Add(new HorizontalStackLayout { Children = { addButton, clearButton } });
            stackLayout.Children.Add(dictionaryEditor);

            return stackLayout;
        }
        public static View CreateListEditor<T>(string bindingPath, string labelText, T defaultElement = default(T), IValueConverter converter = null)
        where T : ICloneable, INotifyPropertyChanged
        {
            var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

            var listEditor = new CollectionView();
            listEditor.ItemTemplate = new DataTemplate(() =>
                {
                    var entry = new Entry();
                    entry.SetBinding(Entry.TextProperty, "Value", BindingMode.TwoWay, converter);

                    var deleteButton = new Button { Text = "-" };
                    deleteButton.Clicked += (sender, e) =>
                    {
                        var property = (listEditor.BindingContext).GetType().GetProperty(bindingPath);
                        if (property.GetValue(listEditor.BindingContext) is ObservableCollection<T> list)
                        {
                            if ((sender as Button).BindingContext is T value)
                            {
                                list.Remove(value);
                            }
                        }
                    };

                    var entryLayout = new HorizontalStackLayout { Children = { entry, deleteButton } };

                    return entryLayout;
                });

            listEditor.SetBinding(ItemsView.ItemsSourceProperty, new Binding(bindingPath, BindingMode.TwoWay));

            var addButton = new Button { Text = "+" };
            addButton.Clicked += (sender, e) =>
            {
                var property = (sender as Button).BindingContext.GetType().GetProperty(bindingPath);
                if (property != null)
                {
                    if (property.GetValue((sender as Button).BindingContext) is ObservableList<T> list)
                    {
                        list.Add((T)defaultElement.Clone());
                    }
                }
            };

            var clearButton = new Button { Text = "Clear" };
            clearButton.Clicked += (sender, e) =>
            {
                var property = (sender as Button).BindingContext.GetType().GetProperty(bindingPath);
                if (property != null)
                {
                    if (property.GetValue((sender as Button).BindingContext) is IList list)
                    {
                        list.Clear();
                    }
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