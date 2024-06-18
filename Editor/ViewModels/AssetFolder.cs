namespace SailorEditor.ViewModels;

public class AssetFolder
{
    public int Id { get; set; }
    public string Name { get; set; }
    public int ParentFolderId { get; set; }
    public int ProjectRootId { get; set; }
}
