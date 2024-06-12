using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;

public class FileIdToFilenameConverter : IValueConverter
{
    private FileId uid;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var assetUID = value as FileId;
        if (string.IsNullOrEmpty(assetUID))
            return null;

        uid = assetUID;

        var AssetService = MauiProgram.GetService<AssetsService>();

        if (!AssetService.Assets.ContainsKey(assetUID))
        {
            return uid;
        }

        return AssetService.Assets[assetUID].DisplayName;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        return uid;
    }
}