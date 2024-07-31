using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;

namespace SailorEditor.Controls;

public class FileIdToPreviewTextureConverter : IValueConverter
{
    private FileId id;
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var fileId = value as FileId;
        if (string.IsNullOrEmpty(fileId))
            return null;

        id = fileId;

        var AssetService = MauiProgram.GetService<AssetsService>();

        if (!AssetService.Assets.TryGetValue(id, out AssetFile outAsset))
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
        return id;
    }
}