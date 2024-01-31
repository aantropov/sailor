using Editor.Models;
using System.IO;

namespace Editor.Services
{
    public class SelectionService
    {
        public static event EventHandler OnSelectionChanged;

        public void OnAssetSelect(AssetFile assetFile)
        {
            OnSelectionChanged?.Invoke(assetFile, null);
        }
    }
}
