using SailorEditor.ViewModels;
using SailorEditor.Services;

namespace SailorEditor.Helpers
{
    public static class HierarchyTreeViewBuilder
    {
        public static TreeViewItemGroup<GameObject, Component> PopulateWorld(WorldService service)
        {
            var world = service.Current;
            var prefabs = service.GameObjects;

            var worldGroup = new TreeViewItemGroup<GameObject, Component>();
            worldGroup.Key = world.Name;

            int groupId = 0;
            foreach (var prefab in prefabs)
            {
                var groups = new List<TreeViewItemGroup<GameObject, Component>>();
                foreach (var gameObject in prefab)
                {
                    var itemGroup = new TreeViewItemGroup<GameObject, Component>();
                    itemGroup.Model = gameObject;
                    itemGroup.GroupId = groupId;
                    itemGroup.Key = gameObject.Name;

                    if (gameObject.ParentIndex == uint.MaxValue)
                    {
                        worldGroup.ChildrenGroups.Add(itemGroup);
                    }

                    groups.Add(itemGroup);
                    groupId++;
                }

                foreach (var gameObject in groups)
                {
                    if (gameObject.Model.ParentIndex != uint.MaxValue)
                    {
                        groups[(int)gameObject.Model.ParentIndex].ChildrenGroups.Add(gameObject);
                    }
                }
            }

            return worldGroup;
        }
    }
}