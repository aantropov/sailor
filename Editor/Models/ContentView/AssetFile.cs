namespace Editor.Models
{
    public class AssetFile
    {
        public int FolderId { get; set; }
        public int Id { get; set; }
        public string Name { get; set; }
        

        public FileInfo FileInfo { get; set; }
    }
}