using System.IO;
using System.Security.Cryptography.X509Certificates;

namespace Editor.ViewModels
{
    public class AssetFile
    {
        public FileInfo Asset { get; set; }
        public FileInfo AssetInfo { get; set; }
        public Dictionary<string, string> Properties { get; set; } = new();

        public string Name { get; set; }
        public int Id { get; set; }
        public int FolderId { get; set; }

        public virtual bool PreloadResources() { return true; }
    }

    public class TextureFile : AssetFile
    {
        public ImageSource ImageSource { get; set; }
        public override bool PreloadResources() 
        {
            if (ImageSource == null)
            {
                ImageSource = ImageSource.FromFile(Asset.FullName);
            }

            return true;
        }
    }
}