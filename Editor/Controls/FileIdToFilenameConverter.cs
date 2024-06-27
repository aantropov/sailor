using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;
using SailorEditor.Utility;

namespace SailorEditor.Controls;

public class FileIdToFilenameConverter : IValueConverter
{
    private FileId uid;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var fileId = value as FileId ?? (value as Observable<FileId>)?.Value;
        if (string.IsNullOrEmpty(fileId))
            return null;

        uid = fileId;

        var AssetService = MauiProgram.GetService<AssetsService>();

        if (!AssetService.Assets.ContainsKey(fileId))
        {
            return uid;
        }

        return AssetService.Assets[fileId].DisplayName;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        return uid;
    }
}