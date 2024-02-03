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

        public static void AddGridRow(Microsoft.Maui.Controls.Grid grid, View view, int rowIndex, Microsoft.Maui.GridLength gridLength)
        {
            if (grid.RowDefinitions.Count <= rowIndex)
            {
                grid.RowDefinitions.Add(new Microsoft.Maui.Controls.RowDefinition { Height = gridLength });
            }
            grid.Children.Add(view);
            Grid.SetRow(view, rowIndex);
        }
        public static void AddGridRowWithLabel(Grid grid, string labelText, View contentView, int rowIndex)
        {
            var label = new Label { Text = labelText, VerticalOptions = LayoutOptions.Center, HorizontalOptions = LayoutOptions.Start };

            grid.Add(label, 0, rowIndex);
            grid.Add(contentView, 1, rowIndex);
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

        public class EnumToStringConverter<TEnum> : IValueConverter where TEnum : struct
        {
            public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
            {
                return value?.ToString();
            }

            public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
            {
                if (value is string stringValue)
                {
                    if (Enum.TryParse<TEnum>(stringValue, out var enumValue))
                    {
                        return enumValue;
                    }
                }

                return default(TEnum);
            }
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