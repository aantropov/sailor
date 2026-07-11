using System.Globalization;

namespace SailorEditor.Controls;

public class IntValueConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => value is int intValue
            ? intValue.ToString(CultureInfo.InvariantCulture)
            : value?.ToString() ?? string.Empty;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => value is string stringValue && int.TryParse(
            stringValue,
            NumberStyles.Integer,
            CultureInfo.InvariantCulture,
            out var result)
            ? result
            : 0;
}
