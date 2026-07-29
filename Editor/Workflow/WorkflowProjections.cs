using System.Collections.ObjectModel;

namespace SailorEditor.Workflow;

public enum SelectionTargetKind
{
    None,
    GameObject,
    Component,
    Asset,
}

public sealed record SelectionSnapshot(string? SelectedId, SelectionTargetKind Kind)
{
    public static SelectionSnapshot Empty { get; } = new(null, SelectionTargetKind.None);
}

public sealed class SelectionStore
{
    public SelectionSnapshot Current { get; private set; } = SelectionSnapshot.Empty;

    public event Action<SelectionSnapshot>? Changed;

    public bool Select(string? selectedId, SelectionTargetKind kind)
    {
        var next = string.IsNullOrWhiteSpace(selectedId) ? SelectionSnapshot.Empty : new SelectionSnapshot(selectedId, kind);
        if (Equals(Current, next))
            return false;

        Current = next;
        Changed?.Invoke(Current);
        return true;
    }

    public bool Clear() => Select(null, SelectionTargetKind.None);
}

public sealed record HierarchySourceItem(
    string Id,
    string Label,
    string? ParentId,
    bool IsPrefabLinked = false,
    string? PrefabFileId = null,
    string? PrefabRootInstanceId = null);

public sealed record HierarchyProjectionNode(
    string Id,
    string Label,
    bool IsSelected,
    bool IsExpanded,
    IReadOnlyList<HierarchyProjectionNode> Children,
    bool IsPrefabLinked = false,
    string? PrefabFileId = null,
    string? PrefabRootInstanceId = null);

public sealed record HierarchyListRow(
    string InstanceId,
    int Depth,
    string Label,
    bool HasChildren,
    bool IsExpanded,
    bool IsSelected,
    bool IsPrefabLinked = false,
    string? PrefabFileId = null,
    string? PrefabRootInstanceId = null)
{
    public string ExpandGlyph => IsExpanded ? "−" : "+";
    public string PrefabLinkGlyph => IsPrefabLinked ? "◆" : string.Empty;
}

public static class HierarchyRowReconciler
{
    public static void Reconcile(
        ObservableCollection<HierarchyListRow> current,
        IReadOnlyList<HierarchyListRow> desired)
    {
        ArgumentNullException.ThrowIfNull(current);
        ArgumentNullException.ThrowIfNull(desired);

        for (var desiredIndex = 0; desiredIndex < desired.Count; desiredIndex++)
        {
            var desiredRow = desired[desiredIndex];
            if (desiredIndex < current.Count &&
                string.Equals(current[desiredIndex].InstanceId, desiredRow.InstanceId, StringComparison.Ordinal))
            {
                if (current[desiredIndex] != desiredRow)
                    current[desiredIndex] = desiredRow;
                continue;
            }

            var existingIndex = -1;
            for (var currentIndex = desiredIndex + 1; currentIndex < current.Count; currentIndex++)
            {
                if (string.Equals(current[currentIndex].InstanceId, desiredRow.InstanceId, StringComparison.Ordinal))
                {
                    existingIndex = currentIndex;
                    break;
                }
            }

            if (existingIndex >= 0)
            {
                current.Move(existingIndex, desiredIndex);
                if (current[desiredIndex] != desiredRow)
                    current[desiredIndex] = desiredRow;
            }
            else
            {
                current.Insert(desiredIndex, desiredRow);
            }
        }

        while (current.Count > desired.Count)
            current.RemoveAt(current.Count - 1);
    }
}

public static class InspectorSelectionIdentity
{
    public static bool AreEquivalent(Type? currentType, string? currentId, Type? nextType, string? nextId)
        => currentType is not null &&
           currentType == nextType &&
           !string.IsNullOrWhiteSpace(currentId) &&
           string.Equals(currentId, nextId, StringComparison.Ordinal);
}

public static class HierarchyProjectionBuilder
{
    const string RootKey = "__root__";

    public static IReadOnlyList<HierarchyProjectionNode> Build(IEnumerable<HierarchySourceItem> sourceItems, ISet<string>? expandedIds, string? selectedId)
    {
        var items = sourceItems.ToArray();
        var itemIds = items.Select(x => x.Id).ToHashSet(StringComparer.Ordinal);
        var childrenLookup = items
            .GroupBy(x => string.IsNullOrWhiteSpace(x.ParentId) || !itemIds.Contains(x.ParentId) ? RootKey : x.ParentId)
            .ToDictionary(x => x.Key, x => x.OrderBy(y => y.Label, StringComparer.Ordinal).ToArray());
        var roots = childrenLookup.TryGetValue(RootKey, out var rootItems) ? rootItems : Array.Empty<HierarchySourceItem>();
        if (roots.Length == 0 && items.Length > 0)
            roots = items.OrderBy(x => x.Label, StringComparer.Ordinal).ToArray();

        return roots.Select(root => BuildNode(root, childrenLookup, expandedIds ?? new HashSet<string>(), selectedId, [])).ToArray();
    }

    public static IReadOnlyList<HierarchyListRow> Flatten(IReadOnlyList<HierarchyProjectionNode> roots)
    {
        if (roots.Count == 0)
            return Array.Empty<HierarchyListRow>();

        var rows = new List<HierarchyListRow>();
        foreach (var root in roots)
            AppendVisibleRows(root, depth: 0, rows);

        return rows;
    }

    static HierarchyProjectionNode BuildNode(HierarchySourceItem item, IReadOnlyDictionary<string, HierarchySourceItem[]> childrenLookup, ISet<string> expandedIds, string? selectedId, HashSet<string> visited)
    {
        if (!visited.Add(item.Id))
        {
            return new HierarchyProjectionNode(
                item.Id,
                item.Label,
                string.Equals(item.Id, selectedId, StringComparison.Ordinal),
                expandedIds.Contains(item.Id),
                Array.Empty<HierarchyProjectionNode>(),
                item.IsPrefabLinked,
                item.PrefabFileId,
                item.PrefabRootInstanceId);
        }

        var children = childrenLookup.TryGetValue(item.Id, out var childItems)
            ? childItems.Select(child => BuildNode(child, childrenLookup, expandedIds, selectedId, new HashSet<string>(visited, StringComparer.Ordinal))).ToArray()
            : Array.Empty<HierarchyProjectionNode>();

        return new HierarchyProjectionNode(
            item.Id,
            item.Label,
            string.Equals(item.Id, selectedId, StringComparison.Ordinal),
            expandedIds.Contains(item.Id),
            children,
            item.IsPrefabLinked,
            item.PrefabFileId,
            item.PrefabRootInstanceId);
    }

    static void AppendVisibleRows(HierarchyProjectionNode node, int depth, ICollection<HierarchyListRow> rows)
    {
        rows.Add(new HierarchyListRow(
            node.Id,
            depth,
            node.Label,
            node.Children.Count > 0,
            node.IsExpanded,
            node.IsSelected,
            node.IsPrefabLinked,
            node.PrefabFileId,
            node.PrefabRootInstanceId));
        if (!node.IsExpanded)
            return;

        foreach (var child in node.Children)
            AppendVisibleRows(child, depth + 1, rows);
    }
}

public sealed record InspectorComponentProjection(string Id, string DisplayName, string TypeName);

public sealed record InspectorProjection(
    string? SelectedId,
    SelectionTargetKind Kind,
    string? DisplayName,
    string? SelectedGameObjectId,
    IReadOnlyList<InspectorComponentProjection> Components)
{
    public static InspectorProjection Empty { get; } = new(null, SelectionTargetKind.None, null, null, Array.Empty<InspectorComponentProjection>());
}

public static class InspectorProjectionBuilder
{
    public static InspectorProjection Build(string? selectedId, SelectionTargetKind kind, string? displayName, string? selectedGameObjectId, IEnumerable<InspectorComponentProjection>? components)
        => string.IsNullOrWhiteSpace(selectedId)
            ? InspectorProjection.Empty
            : new InspectorProjection(selectedId, kind, displayName, selectedGameObjectId, components?.ToArray() ?? Array.Empty<InspectorComponentProjection>());
}
