using Editor.Models;
using System.IO;

namespace Editor.Services
{
    public class SelectionService
    {
        public event Action<AssetFile> OnSelectAssetAction = delegate { };

        public void OnSelectAsset(AssetFile assetFile)
        {
            OnSelectAssetAction?.Invoke(assetFile);
        }
    }
}
