using System.Globalization;

namespace SailorEditor.Utility
{
    public class EnumToStringConverter<TEnum> : IValueConverter where TEnum : struct
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture) => value?.ToString();

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
}
