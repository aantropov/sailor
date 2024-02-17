using CommunityToolkit.Mvvm.ComponentModel;
using System.Globalization;

namespace SailorEditor.Utility
{
    public partial class ObservableString : ObservableObject
    {
        public ObservableString(string v)
        {
            Str = v;
        }
        public override string ToString() => Str;
        public override bool Equals(object obj)
        {
            if (obj is ObservableString other)
            {
                return String.Equals(Str, other.Str, StringComparison.Ordinal);
            }

            return false;
        }

        public override int GetHashCode()
        {
            return Str?.GetHashCode() ?? 0;
        }

        [ObservableProperty]
        private string _str;
    }

    public class ObservableStringConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            return value?.ToString();
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is string stringValue)
            {
                return new ObservableString(stringValue);
            }

            return default(ObservableString);
        }
    }
}
