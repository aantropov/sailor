namespace Editor.Models
{
    [Serializable]
    public class TreeViewItem
    {
        public string Key { get; set; }
        public int ItemId { get; set; }
    }

    [Serializable]
    public class TreeViewItemGroup
    {
        public List<TreeViewItemGroup> Children { get; } = new();
        public List<TreeViewItem> XamlItems { get; } = new();

        public string Name { get; set; }
        public int GroupId { get; set; }
    }
}
