namespace SailorEditor.Utility;

public sealed record ListSelectionResult(
    IReadOnlyList<string> SelectedIds,
    string? AnchorId,
    string? PrimaryId);

public static class ListSelectionPolicy
{
    public static ListSelectionResult Select(
        IReadOnlyList<string> visibleIds,
        IEnumerable<string> selectedIds,
        string clickedId,
        string? anchorId,
        bool extendRange = false,
        bool additive = false,
        bool preserveSelected = false)
    {
        var selected = selectedIds.Distinct(StringComparer.Ordinal).ToList();
        if (preserveSelected && selected.Contains(clickedId, StringComparer.Ordinal))
        {
            return new(selected, anchorId, selected.LastOrDefault());
        }

        if (extendRange)
        {
            var start = IndexOf(visibleIds, anchorId);
            var end = IndexOf(visibleIds, clickedId);
            if (start >= 0 && end >= 0)
            {
                var range = visibleIds.Skip(Math.Min(start, end))
                    .Take(Math.Abs(end - start) + 1);
                var result = additive ? selected.Concat(range) : range;
                return new(WithPrimary(result, clickedId), anchorId, clickedId);
            }

            // A collapsed anchor is retained so expanding its parent restores range selection.
            return new(WithPrimary(additive ? selected.Append(clickedId) : [clickedId], clickedId),
                anchorId ?? clickedId, clickedId);
        }

        if (additive && selected.Remove(clickedId))
        {
            return new(selected, clickedId, selected.LastOrDefault());
        }

        return new(WithPrimary(additive ? selected.Append(clickedId) : [clickedId], clickedId),
            clickedId, clickedId);
    }

    static IReadOnlyList<string> WithPrimary(IEnumerable<string> ids, string primary)
        => ids.Where(id => !string.Equals(id, primary, StringComparison.Ordinal)).Append(primary)
            .Distinct(StringComparer.Ordinal).ToArray();

    static int IndexOf(IReadOnlyList<string> ids, string? id)
    {
        for (var index = 0; index < ids.Count; index++)
        {
            if (string.Equals(ids[index], id, StringComparison.Ordinal))
            {
                return index;
            }
        }
        return -1;
    }
}
