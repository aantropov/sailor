
namespace SailorEditor.ViewModels;

[Serializable]
public abstract class TreeViewItemBase
{
    public string Key { get; set; }
    public int ItemId { get; set; }

    public abstract TModel GetModel<TModel>();
}

[Serializable]
public class TreeViewItem<TModel> : TreeViewItemBase
{
    public TModel Model { get; set; }

    public override TModel1 GetModel<TModel1>()
    {
        if (Model is TModel1 model)
        {
            return model;
        }

        throw new InvalidCastException($"Model is not of type {typeof(TModel1)}");
    }
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
    public List<TreeViewItemGroup<TGroupModel, TItemModel>> ChildrenGroups { get; } = [];
    public List<TreeViewItem<TItemModel>> ChildrenItems { get; } = [];

    public TGroupModel Model { get; set; }
    public override IEnumerable<TreeViewItemGroup<TGroupModel, TItemModel>> ChildrenGroupsBase { get => ChildrenGroups; }
    public override IEnumerable<TreeViewItem<TItemModel>> ChildrenItemsBase { get => ChildrenItems; }
}
