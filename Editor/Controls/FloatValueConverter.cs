using System.Globalization;

namespace SailorEditor.Controls;

public class FloatValueConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        if (value is float floatValue)
        {
            return floatValue.ToString(CultureInfo.InvariantCulture);
        }
        return value.ToString();
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        if (value is string stringValue && float.TryParse(stringValue, NumberStyles.Any, CultureInfo.InvariantCulture, out float result))
        {
            return result;
        }
        return 0;
    }
}
