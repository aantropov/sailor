using System.Globalization;
using SailorEditor.Utility;

namespace SailorEditor.Controls;

public class FloatValueConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => value is InspectorAggregateField aggregate
            ? aggregate.DisplayValue
            : value is float floatValue
            ? floatValue.ToString(CultureInfo.InvariantCulture)
            : value?.ToString() ?? string.Empty;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => value is string stringValue && float.TryParse(
            stringValue,
            NumberStyles.Float,
            CultureInfo.InvariantCulture,
            out var result) &&
            float.IsFinite(result)
            ? result
            : BindableProperty.UnsetValue;
}
