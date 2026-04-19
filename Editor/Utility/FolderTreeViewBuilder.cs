using SailorEditor.Controls;
using SailorEditor.Content;
using SailorEditor.ViewModels;

namespace SailorEditor.Helpers
{
    public static class FolderTreeViewBuilder
    {
        private static TreeViewItemGroup<AssetFolder, AssetFile>? FindParentFolder(TreeViewItemGroup<AssetFolder, AssetFile> group, AssetFolder folder)
        {
            if (group.GroupId == folder.ParentFolderId)
                return group;

            foreach (var currentGroup in group.ChildrenGroups)
            {
                var search = FindParentFolder(currentGroup, folder);
                if (search != null)
                    return search;
            }

            return null;
        }

        public static TreeViewNode? FindItemRecursive<T>(this TreeViewNode parent, T id)
            where T : IComparable<T>
        {
            if (parent.BindingContext is TreeViewItem<T> item && item.Model.Equals(id))
            {
                return parent;
            }

            if (parent.BindingContext is TreeViewItemGroup<AssetFolder, AssetFile>)
            {
                foreach (var el in parent.ChildrenList)
                {
                    var res = FindItemRecursive(el, id);
                    if (res != null)
                        return res;
                }
            }

            return null;
        }

        public static TreeViewNode? FindFolderRecursive(this TreeViewNode parent, int folderId)
        {
            if (parent.BindingContext is TreeViewItemGroup<AssetFolder, AssetFile> folder && folder.Model?.Id == folderId)
            {
                return parent;
            }

            if (parent.BindingContext is TreeViewItemGroup<AssetFolder, AssetFile>)
            {
                foreach (var el in parent.ChildrenList)
                {
                    var res = FindFolderRecursive(el, folderId);
                    if (res != null)
                        return res;
                }
            }

            return null;
        }

        public static TreeViewNode? FindFileRecursive(this TreeViewNode parent, AssetFile id)
        {
            if (parent.BindingContext is TreeViewItem<AssetFile> assetFile && assetFile.Model.FileId == id.FileId)
            {
                return parent;
            }

            if (parent.BindingContext is TreeViewItemGroup<AssetFolder, AssetFile>)
            {
                foreach (var el in parent.ChildrenList)
                {
                    var res = FindFileRecursive(el, id);
                    if (res != null)
                        return res;
                }
            }

            return null;
        }

        public static TreeViewItemGroup<AssetFolder, AssetFile> PopulateDirectory(
            ProjectContentProjection projection,
            IReadOnlyDictionary<int, AssetFolder> foldersById,
            IReadOnlyDictionary<string, AssetFile> assetsByFileId)
        {
            var projectRootGroup = new TreeViewItemGroup<AssetFolder, AssetFile>
            {
                Key = projection.Root.Name
            };

            var groupsById = new Dictionary<int, TreeViewItemGroup<AssetFolder, AssetFile>>();
            foreach (var projectionFolder in projection.Folders.OrderBy(x => x.FullPath, StringComparer.OrdinalIgnoreCase))
            {
                if (projectionFolder.FolderId is not { } folderId || !foldersById.TryGetValue(folderId, out var folder))
                {
                    continue;
                }

                var itemGroup = new TreeViewItemGroup<AssetFolder, AssetFile>
                {
                    Model = folder,
                    GroupId = folder.Id,
                    Key = projectionFolder.Name
                };

                groupsById[folder.Id] = itemGroup;
                if (folder.ParentFolderId == -1)
                {
                    projectRootGroup.ChildrenGroups.Add(itemGroup);
                    continue;
                }

                if (groupsById.TryGetValue(folder.ParentFolderId, out var parentGroup))
                {
                    parentGroup.ChildrenGroups.Add(itemGroup);
                    continue;
                }

                parentGroup = projectRootGroup.ChildrenGroups
                    .Select(group => FindParentFolder(group, folder))
                    .FirstOrDefault(group => group != null);

                if (parentGroup != null)
                {
                    parentGroup.ChildrenGroups.Add(itemGroup);
                }
            }

            foreach (var projectionAsset in projection.VisibleAssets)
            {
                if (!assetsByFileId.TryGetValue(projectionAsset.FileId, out var file))
                {
                    continue;
                }

                var item = new TreeViewItem<AssetFile>
                {
                    Model = file,
                    ItemId = file.Id,
                    Key = projectionAsset.DisplayName
                };

                if (projectionAsset.FolderId == -1)
                {
                    projectRootGroup.ChildrenItems.Add(item);
                    continue;
                }

                if (groupsById.TryGetValue(projectionAsset.FolderId, out var folderGroup))
                {
                    folderGroup.ChildrenItems.Add(item);
                }
            }

            return projectRootGroup;
        }
    }
}
