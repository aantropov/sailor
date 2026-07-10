namespace SailorEditor.ViewModels;

public class AssetFolder
{
    public int Id { get; set; }
    public string Name { get; set; } = string.Empty;
    public int ParentFolderId { get; set; }
    public int ProjectRootId { get; set; }
    public string FullPath { get; set; } = string.Empty;
    public bool IsReadOnly { get; set; }
}
