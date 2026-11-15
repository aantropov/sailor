using SailorEditor.Utility;

namespace SailorEditor.Tests;

public sealed class ListSelectionPolicyTests
{
    static readonly string[] Visible = ["a", "b", "c", "d", "e"];

    [Fact]
    public void OrdinaryClickReplacesSelectionAndMovesAnchor()
    {
        var result = ListSelectionPolicy.Select(Visible, ["a", "b"], "d", "a");
        Assert.Equal(["d"], result.SelectedIds);
        Assert.Equal("d", result.AnchorId);
        Assert.Equal("d", result.PrimaryId);
    }

    [Fact]
    public void ShiftRangeFollowsVisibleOrderAndRetainsAnchorAcrossRepeatedClicks()
    {
        var result = ListSelectionPolicy.Select(Visible, ["d"], "b", "d", extendRange: true);
        Assert.Equal(["c", "d", "b"], result.SelectedIds);
        var shorter = ListSelectionPolicy.Select(Visible, result.SelectedIds, "c", result.AnchorId, extendRange: true);
        Assert.Equal(["d", "c"], shorter.SelectedIds);
        Assert.Equal("d", shorter.AnchorId);
    }

    [Fact]
    public void AdditiveClickTogglesWithoutDroppingOtherRows()
    {
        var added = ListSelectionPolicy.Select(Visible, ["a"], "c", "a", additive: true);
        Assert.Equal(["a", "c"], added.SelectedIds);
        var removed = ListSelectionPolicy.Select(Visible, added.SelectedIds, "c", added.AnchorId, additive: true);
        Assert.Equal(["a"], removed.SelectedIds);
        Assert.Equal("a", removed.PrimaryId);
    }

    [Fact]
    public void AdditiveRangePreservesHiddenSelection()
    {
        var result = ListSelectionPolicy.Select(Visible, ["hidden", "b"], "d", "b", extendRange: true, additive: true);
        Assert.Equal(["hidden", "b", "c", "d"], result.SelectedIds);
    }

    [Fact]
    public void CollapsedAnchorIsKeptUntilItBecomesVisibleAgain()
    {
        var collapsed = ListSelectionPolicy.Select(["a", "d"], ["b"], "d", "b", extendRange: true);
        Assert.Equal("b", collapsed.AnchorId);
        var expanded = ListSelectionPolicy.Select(Visible, collapsed.SelectedIds, "e", collapsed.AnchorId, extendRange: true);
        Assert.Equal(["b", "c", "d", "e"], expanded.SelectedIds);
    }

    [Fact]
    public void DragOrContextClickOnSelectedRowPreservesGroupAndPrimary()
    {
        var result = ListSelectionPolicy.Select(Visible, ["a", "c"], "c", "a", preserveSelected: true);
        Assert.Equal(["a", "c"], result.SelectedIds);
        Assert.Equal("c", result.PrimaryId);
        var unselected = ListSelectionPolicy.Select(Visible, ["a", "c"], "d", "a", preserveSelected: true);
        Assert.Equal(["d"], unselected.SelectedIds);
    }

    [Fact]
    public void ShiftClickThenPlainClickReplacesSelection()
    {
        // Simulate: Click A (plain), Shift-Click D, Release Shift, Click B
        var step1 = ListSelectionPolicy.Select(Visible, [], "a", null);
        var step2 = ListSelectionPolicy.Select(Visible, step1.SelectedIds, "d", step1.AnchorId, extendRange: true);
        // Release shift: next click is plain (extendRange: false, additive: false)
        var step3 = ListSelectionPolicy.Select(Visible, step2.SelectedIds, "b", step2.AnchorId, extendRange: false, additive: false);

        Assert.Equal(["b"], step3.SelectedIds);
        Assert.Equal("b", step3.AnchorId);
        Assert.Equal("b", step3.PrimaryId);
    }

    [Fact]
    public void AdditiveClickThenPlainClickReplacesSelection()
    {
        // Simulate: Click A (plain), Ctrl-Click C, Release Ctrl, Click E
        var step1 = ListSelectionPolicy.Select(Visible, [], "a", null);
        var step2 = ListSelectionPolicy.Select(Visible, step1.SelectedIds, "c", step1.AnchorId, additive: true);
        // Release ctrl: next click is plain
        var step3 = ListSelectionPolicy.Select(Visible, step2.SelectedIds, "e", step2.AnchorId, extendRange: false, additive: false);

        Assert.Equal(["e"], step3.SelectedIds);
        Assert.Equal("e", step3.AnchorId);
    }

    [Fact]
    public void ShiftAdditiveRangeThenPlainClickReplacesSelection()
    {
        // Simulate: Click A, Ctrl-Click B, Shift-Ctrl-Click D, Release both, Click E
        var step1 = ListSelectionPolicy.Select(Visible, [], "a", null);
        var step2 = ListSelectionPolicy.Select(Visible, step1.SelectedIds, "b", step1.AnchorId, additive: true);
        var step3 = ListSelectionPolicy.Select(Visible, step2.SelectedIds, "d", step2.AnchorId, extendRange: true, additive: true);
        // Release both: next click is plain
        var step4 = ListSelectionPolicy.Select(Visible, step3.SelectedIds, "e", step3.AnchorId, extendRange: false, additive: false);

        Assert.Equal(["e"], step4.SelectedIds);
        Assert.Equal("e", step4.AnchorId);
    }
}
