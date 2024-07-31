using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;
using SailorEditor.Utility;

namespace SailorEditor.Controls;

public class FileIdToFilenameConverter : IValueConverter
{
    private FileId id;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var fileId = value as FileId ?? (value as Observable<FileId>)?.Value;
        if (string.IsNullOrEmpty(fileId))
            return null;

        id = fileId;

        var AssetService = MauiProgram.GetService<AssetsService>();

        if (!AssetService.Assets.ContainsKey(fileId))
        {
            return id;
        }

        return AssetService.Assets[fileId].DisplayName;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        return id;
    }
}