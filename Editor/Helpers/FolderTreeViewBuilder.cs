using Editor.Models;
using Editor.Services;

namespace Editor.Helpers
{
    public class FolderTreeViewBuilder
    {
        private TreeViewItemGroup FindParentFolder(TreeViewItemGroup group, AssetFolder folder)
        {
            if (group.GroupId == folder.ParentFolderId)
                return group;

            if (group.Children != null)
            {
                foreach (var currentGroup in group.Children)
                {
                    var search = FindParentFolder(currentGroup, folder);

                    if (search != null)
                        return search;
                }
            }

            return null;
        }

        public TreeViewItemGroup GroupData(AssetsService service)
        {
            var projectRoot = service.Root;
            var folders = service.Folders.OrderBy(x => x.ParentFolderId);
            var assets = service.Files;

            var projectRootGroup = new TreeViewItemGroup();
            projectRootGroup.Name = projectRoot.Name;

            foreach (var dept in folders)
            {
                var itemGroup = new TreeViewItemGroup();
                itemGroup.Name = dept.Name;
                itemGroup.GroupId = dept.Id;

                // Assets first
                var assetsFolder = assets.Where(x => x.FolderId == dept.Id);

                foreach (var emp in assetsFolder)
                {
                    var item = new TreeViewItem();
                    item.ItemId = emp.Id;
                    item.Key = emp.Name;

                    itemGroup.XamlItems.Add(item);
                }

                // Folders now
                if (dept.ParentFolderId == -1)
                {
                    projectRootGroup.Children.Add(itemGroup);
                }
                else
                {
                    TreeViewItemGroup parentGroup = null;

                    foreach (var group in projectRootGroup.Children)
                    {
                        parentGroup = FindParentFolder(group, dept);

                        if (parentGroup != null)
                        {
                            parentGroup.Children.Add(itemGroup);
                            break;
                        }
                    }
                }
            }

            return projectRootGroup;
        }
    }
}