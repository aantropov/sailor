using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;

namespace SailorEngine.Controls;

public class FileIdToTextureConverter : IValueConverter
{
    private FileId uid;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var assetUID = value as FileId;
        if (string.IsNullOrEmpty(assetUID))
            return null;

        uid = assetUID;

        var AssetService = MauiProgram.GetService<AssetsService>();

        if (!AssetService.Assets.TryGetValue(uid, out AssetFile outAsset))
        {
            return null;
        }

        if (outAsset is TextureFile texture)
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