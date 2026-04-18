using SailorEditor.ViewModels;

namespace SailorEditor.Utility;

public static class EditorDragDrop
{
    public const string DragItemKey = "DragItem";
}

public sealed class TreeViewDropRequest
{
    public object Source { get; init; }
    public object Target { get; init; }
    public bool Handled { get; set; }
}
