namespace SailorEditor.Utility;

public sealed class EditorSelectionDragPayload
{
    public EditorSelectionDragPayload(IEnumerable<object> items)
    {
        ArgumentNullException.ThrowIfNull(items);
        Items = Array.AsReadOnly(items.ToArray());
        if (Items.Count < 2 || Items.Any(item => item is null))
        {
            throw new ArgumentException("A selection drag must contain at least two items.", nameof(items));
        }
    }

    public IReadOnlyList<object> Items { get; }
}
