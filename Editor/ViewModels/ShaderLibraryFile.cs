
namespace SailorEditor.ViewModels
{
    public class ShaderLibraryFile : AssetFile
    {
        public string Code { get; set; }

        public override async Task<bool> PreloadResources(bool force)
        {
            if (IsLoaded && !force)
                return true;

            Code = File.ReadAllText(Asset.FullName);

            IsLoaded = true;
            return true;
        }
    }
}