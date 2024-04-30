using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.ComponentModel;
using System.Globalization;

public class AssetUIDToFilenameConverter : IValueConverter
{
    private AssetUID uid;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var assetUID = value as AssetUID;
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