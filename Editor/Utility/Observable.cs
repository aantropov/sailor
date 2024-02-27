using CommunityToolkit.Mvvm.ComponentModel;
using System;
using System.Globalization;

namespace SailorEditor.Utility
{
    public partial class Observable<T> : ObservableObject, ICloneable
        where T : IComparable<T>
    {
        public Observable(T v)
        {
            Value = v;
        }

        public static implicit operator Observable<T>(T value) => new Observable<T>(value);
        public static implicit operator T(Observable<T> observable) => observable.Value;
        public object Clone() => new Observable<T>(Value);
        public override string ToString() => Value.ToString();
        public override bool Equals(object obj)
        {
            if (obj is Observable<T> other)
            {
                return Value.CompareTo(other.Value) == 0;
            }

            return false;
        }
        public override int GetHashCode() => Value?.GetHashCode() ?? 0;

        [ObservableProperty]
        private T value;
    }

    public class ObservableConverter<T> : IValueConverter
        where T : IComparable<T>
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            return ((T)value).ToString();
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is T tValue)
            {
                return new Observable<T>(tValue);
            }

            return default(Observable<T>);
        }
    }
}
