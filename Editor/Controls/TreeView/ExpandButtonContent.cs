namespace SailorEditor.Controls
{
    public class ExpandButtonContent : ContentView
    {
        protected override void OnBindingContextChanged()
        {
            base.OnBindingContextChanged();

            var node = BindingContext as TreeViewNode;
            bool isLeafNode = (node.ChildrenList == null || node.ChildrenList.Count == 0);

            if (isLeafNode && !node.ShowExpandButtonIfEmpty)
            {
                Content = new ResourceImage
                {
                    Resource = isLeafNode ? "blank.png" : "folder_open.png",
                    HeightRequest = 16,
                    WidthRequest = 16,
                };
            }
            else
            {
                Content = new ResourceImage
                {
                    Resource = node.IsExpanded ? "openglyph.png" : "collpsedglyph.png",
                    HeightRequest = 16,
                    WidthRequest = 16
                };
            }
        }
    }
}
