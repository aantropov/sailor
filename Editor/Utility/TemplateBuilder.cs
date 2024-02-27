using SailorEditor.ViewModels;
using SailorEditor.Services;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml;
using System.Runtime.InteropServices;
using WinRT;
using Window = Microsoft.Maui.Controls.Window;
using Microsoft.Maui.Controls;
using Microsoft.UI.Windowing;
using System.Globalization;
using CheckBox = Microsoft.Maui.Controls.CheckBox;
using Grid = Microsoft.Maui.Controls.Grid;
using SkiaSharp;
using System.Linq;
using Microsoft.Maui.Controls.PlatformConfiguration.AndroidSpecific;
using Entry = Microsoft.Maui.Controls.Entry;
using System.Collections.Generic;
using System.Collections;
using System.Runtime.CompilerServices;
using DataTemplate = Microsoft.Maui.Controls.DataTemplate;
using Button = Microsoft.Maui.Controls.Button;
using System.Collections.ObjectModel;
using Microsoft.UI.Xaml.Data;
using BindingMode = Microsoft.Maui.Controls.BindingMode;
using Binding = Microsoft.Maui.Controls.Binding;
using IValueConverter = Microsoft.Maui.Controls.IValueConverter;
using SailorEditor.Utility;
using System.ComponentModel;
using YamlDotNet.Core.Tokens;

namespace SailorEditor.Helpers
{
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
        public static View CreateDictionaryEditor<K, V>(string bindingPath, string labelText, K defaultKey = default(K), V defaultValue = default(V))
        where K : ICloneable, INotifyPropertyChanged
        where V : ICloneable, INotifyPropertyChanged
        {
            var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, FontAttributes = FontAttributes.Bold };

            var dictionaryEditor = new CollectionView();
            dictionaryEditor.ItemTemplate = new DataTemplate(() =>
                {
                    var keyEntry = new Entry();
                    keyEntry.SetBinding(Entry.TextProperty, "Key", BindingMode.TwoWay);

                    var valueEntry = new Entry();
                    valueEntry.SetBinding(Entry.TextProperty, "Value", BindingMode.TwoWay);

                    var deleteButton = new Button { Text = "-" };
                    deleteButton.Clicked += (sender, e) =>
                    {
                        var dictionaryProperty = (dictionaryEditor.BindingContext).GetType().GetProperty(bindingPath);
                        if (dictionaryProperty != null)
                        {
                            if (dictionaryProperty.GetValue(dictionaryEditor.BindingContext) is IDictionary dictionary)
                            {
                                var keyToRemove = keyEntry.Text.ToString();

                                if (dictionary.Contains(keyToRemove))
                                {
                                    dictionary.Remove(keyToRemove);
                                }
                            }
                        }
                    };

                    var entryLayout = new HorizontalStackLayout { Children = { keyEntry, valueEntry, deleteButton } };

                    return entryLayout;
                });

            dictionaryEditor.SetBinding(ItemsView.ItemsSourceProperty, new Binding(bindingPath, BindingMode.TwoWay));

            var addButton = new Button { Text = "+" };
            addButton.Clicked += (sender, e) =>
            {
                var property = (sender as Button).BindingContext.GetType().GetProperty(bindingPath);
                if (property != null)
                {
                    if (property.GetValue((sender as Button).BindingContext) is IDictionary<K, V> dict)
                    {
                        dict[(K)defaultKey.Clone()] = (V)defaultValue.Clone();
                    }
                }
            };

            var clearButton = new Button { Text = "Clear" };
            clearButton.Clicked += (sender, e) =>
            {
                var property = (sender as Button).BindingContext.GetType().GetProperty(bindingPath);
                if (property != null)
                {
                    if (property.GetValue((sender as Button).BindingContext) is ICollection<KeyValuePair<K, V>> dict)
                    {
                        dict.Clear();
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
                    entry.SetBinding(Entry.TextProperty, "Value", BindingMode.TwoWay);

                    var deleteButton = new Button { Text = "-" };
                    deleteButton.Clicked += (sender, e) =>
                    {
                        var property = (listEditor.BindingContext).GetType().GetProperty(bindingPath);
                        if (property != null)
                        {
                            if (property.GetValue(listEditor.BindingContext) is ObservableCollection<T> list)
                            {
                                var valueToRemove = entry.Text.ToString();

                                if (converter != null)
                                {
                                    var value = (T)converter.ConvertBack(valueToRemove, typeof(T), valueToRemove, CultureInfo.CurrentUICulture);
                                    list.Remove(value);
                                }
                                else
                                {
                                    ((IList)list).Remove(valueToRemove);
                                }
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