namespace Editor.Models
{
    [Serializable]
    public abstract class TreeViewItemBase
    {
        public string Key { get; set; }
        public int ItemId { get; set; }
    }

    [Serializable]
    public class TreeViewItem<TModel> : TreeViewItemBase
    {
        public TModel Model { get; set; }
    }

    [Serializable]
    abstract public class TreeViewItemGroupBase
    {
        abstract public IEnumerable<TreeViewItemGroupBase> ChildrenGroupsBase { get; }
        abstract public IEnumerable<TreeViewItemBase> ChildrenItemsBase { get; }

        public string Key { get; set; }
        public int GroupId { get; set; }
    }

    [Serializable]
    public class TreeViewItemGroup<TGroupModel, TItemModel> : TreeViewItemGroupBase
    {
        public List<TreeViewItemGroup<TGroupModel, TItemModel>> ChildrenGroups { get; } = new();
        public List<TreeViewItem<TItemModel>> ChildrenItems { get; } = new();

        public TGroupModel Model { get; set; }
        public override IEnumerable<TreeViewItemGroup<TGroupModel, TItemModel>> ChildrenGroupsBase { get => ChildrenGroups; }
        public override IEnumerable<TreeViewItem<TItemModel>> ChildrenItemsBase { get => ChildrenItems; }
    }
}
