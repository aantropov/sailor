using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;
using SailorEditor.Utility;

namespace SailorEditor.Controls;

public class ObservableConverter : IValueConverter
{
    Observable<string> obj;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        obj = value as Observable<string>;
        if (obj == null)
            return null;

        return obj.Value;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        obj.Value = value.ToString();
        return value;
    }
}
