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
                var groupsByPrefabIndex = new Dictionary<int, TreeViewItemGroup<GameObject, Component>>();

                for (var gameObjectIndex = 0; gameObjectIndex < prefab.Count; gameObjectIndex++)
                {
                    var gameObject = prefab[gameObjectIndex];
                    var itemGroup = new TreeViewItemGroup<GameObject, Component>
                    {
                        Model = gameObject,
                        GroupId = groupId,
                        Key = gameObject.Name
                    };

                    groupsByPrefabIndex[gameObjectIndex] = itemGroup;
                    groupId++;
                }

                for (var gameObjectIndex = 0; gameObjectIndex < prefab.Count; gameObjectIndex++)
                {
                    var gameObject = prefab[gameObjectIndex];
                    var itemGroup = groupsByPrefabIndex[gameObjectIndex];

                    if (gameObject.ParentIndex == uint.MaxValue)
                    {
                        worldGroup.ChildrenGroups.Add(itemGroup);
                    }
                    else if (groupsByPrefabIndex.TryGetValue((int)gameObject.ParentIndex, out var parentGroup))
                    {
                        parentGroup.ChildrenGroups.Add(itemGroup);
                    }
                }
            }

            return worldGroup;
        }
    }
}
