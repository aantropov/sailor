using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;
using SailorEditor.Utility;

namespace SailorEditor.Controls;

public class InstanceIdToDisplayNameConverter : IValueConverter
{
    private InstanceId id;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var instanceId = value as InstanceId ?? (value as Observable<InstanceId>)?.Value;
        if (string.IsNullOrEmpty(instanceId))
            return null;

        id = instanceId;

        if (id.IsEmpty())
        {
            return id;
        }

        var worldService = MauiProgram.GetService<WorldService>();
        return worldService.GetComponent(id).DisplayName;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        return id;
    }
}