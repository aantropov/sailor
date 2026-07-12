using System.Globalization;

namespace SailorEditor.Controls;

public class UIntValueConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => value is uint uintValue
            ? uintValue.ToString(CultureInfo.InvariantCulture)
            : value?.ToString() ?? string.Empty;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => value is string stringValue && uint.TryParse(
            stringValue,
            NumberStyles.Integer,
            CultureInfo.InvariantCulture,
            out var result)
            ? result
            : BindableProperty.UnsetValue;
}
