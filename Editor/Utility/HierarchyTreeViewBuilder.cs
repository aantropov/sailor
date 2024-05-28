using SailorEditor.ViewModels;
using SailorEditor.Services;

namespace SailorEditor.Helpers
{
    public static class HierarchyTreeViewBuilder
    {
        private static TreeViewItemGroup<GameObject, Component> FindParentGameObject(TreeViewItemGroup<GameObject, Component> group, GameObject folder)
        {
            if (group.GroupId == folder.ParentGameObjectId)
                return group;

            if (group.ChildrenGroups != null)
            {
                foreach (var currentGroup in group.ChildrenGroups)
                {
                    var search = FindParentGameObject(currentGroup, folder);

                    if (search != null)
                        return search;
                }
            }

            return null;
        }

        public static TreeViewItemGroup<GameObject, Component> PopulateWorld(WorldService service)
        {
            var world = service.Current;
            //var gameObjects = service.GameObjects.OrderBy(x => x.ParentGameObjectId);
            //var components = service.Components;

            var gameObjects = new List<GameObject>
            {
                new GameObject { ParentGameObjectId = -1, Id = 0, WorldId = 0, Name = "Untitled 1" },
                new GameObject { ParentGameObjectId = 0, Id = 1, WorldId = 0, Name = "Untitled 2" }
            };

            var components = new List<Component>
            {
                new Component { GameObjectId = 0, Id = 0, DisplayName = "Component 1" },
                new Component { GameObjectId = 0, Id = 1, DisplayName = "Component 2" }
            };

            var worldGroup = new TreeViewItemGroup<GameObject, Component>();
            worldGroup.Key = world.Name;

            foreach (var gameObject in gameObjects)
            {
                var itemGroup = new TreeViewItemGroup<GameObject, Component>();
                itemGroup.Model = gameObject;
                itemGroup.GroupId = gameObject.Id;
                itemGroup.Key = gameObject.Name;

                var gameObjectComponents = components.Where(x => x.GameObjectId == gameObject.Id);

                foreach (var file in gameObjectComponents)
                {
                    var item = new TreeViewItem<Component>();
                    item.Model = file;
                    item.ItemId = file.Id;
                    item.Key = file.DisplayName;

                    itemGroup.ChildrenItems.Add(item);
                }

                if (gameObject.ParentGameObjectId == -1)
                {
                    worldGroup.ChildrenGroups.Add(itemGroup);
                }
                else
                {
                    TreeViewItemGroup<GameObject, Component> parentGroup = null;

                    foreach (var group in worldGroup.ChildrenGroups)
                    {
                        parentGroup = FindParentGameObject(group, gameObject);

                        if (parentGroup != null)
                        {
                            parentGroup.ChildrenGroups.Add(itemGroup);
                            break;
                        }
                    }
                }
            }

            return worldGroup;
        }
    }
}