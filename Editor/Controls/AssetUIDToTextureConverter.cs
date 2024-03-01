using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.ComponentModel;
using System.Globalization;

public class AssetUIDToTextureConverter : IValueConverter
{
    private string uid;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var assetUID = value as string;
        if (string.IsNullOrEmpty(assetUID))
            return null;

        uid = assetUID;

        var AssetService = MauiProgram.GetService<AssetsService>();

        if (!AssetService.Assets.ContainsKey(assetUID))
        {
            return null;
        }

        if (AssetService.Assets[assetUID] is TextureFile texture)
        {
            return texture.Texture;
        }

        return null;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
    {
        return uid;
    }
}