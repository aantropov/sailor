using System.Security.Cryptography.X509Certificates;

namespace Editor.Models
{
    public class AssetFile
    {
        public FileInfo FileInfo { get; set; }

        public string Name { get; set; }
        public int Id { get; set; }
        public int FolderId { get; set; }
    }
}