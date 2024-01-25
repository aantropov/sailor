namespace Editor.Controls
{
    public class ExpandButtonContent : ContentView
    {
        protected override void OnBindingContextChanged()
        {
            base.OnBindingContextChanged();

            var node = BindingContext as TreeViewNode;
            bool isLeafNode = (node.ChildrenList == null || node.ChildrenList.Count == 0);

            //empty nodes have no icon to expand unless showExpandButtonIfEmpty is et to true which will show the expand
            //icon can click and populated node on demand propably using the expand event.
            if ((isLeafNode) && !node.ShowExpandButtonIfEmpty)
            {
                Content = new ResourceImage
                {
                    Resource = isLeafNode ? "FolderTreeView/blank.png" : "FolderTreeView/folderopen.png",
                    HeightRequest = 16,
                    WidthRequest = 16
                };
            }
            else
            {
                Content = new ResourceImage
                {
                    Resource = node.IsExpanded ? "FolderTreeView/openglyph.png" : "FolderTreeView/collpsedglyph.png",
                    HeightRequest = 16,
                    WidthRequest = 16
                };
            }
        }
    }
}
