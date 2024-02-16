using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.ComponentModel;
using System.Globalization;

public class AssetUIDToFilenameConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var assetUID = value as string;
        if (string.IsNullOrEmpty(assetUID))
            return null;

        var AssetService = MauiProgram.GetService<AssetsService>();
        return AssetService.Assets[assetUID].DisplayName;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}