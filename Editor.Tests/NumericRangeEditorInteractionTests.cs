using System.Globalization;
using SailorEditor.Utility;
using SailorEngine;

namespace SailorEditor.Editor.Tests;

public sealed class NumericRangeEditorInteractionTests
{
    [Fact]
    public void ModelSynchronization_ClampsOnlyTheSliderDisplayWithoutApplyingTheValue()
    {
        var interaction = new NumericRangeSliderInteraction(
            new NumericPropertyRange(0, 10));
        var loadedModelValue = 25.0;

        var displayedValue = interaction.BeginModelSynchronization(loadedModelValue);
        var decision = interaction.HandleValueChanged(displayedValue);
        interaction.EndModelSynchronization();

        Assert.Equal(10, displayedValue);
        Assert.False(decision.ShouldApply);
        Assert.Equal(25, loadedModelValue);
    }

    [Fact]
    public void Drag_AppliesOnlyTheLastValueAtCompletion()
    {
        var interaction = new NumericRangeSliderInteraction(
            new NumericPropertyRange(0, 10));

        interaction.BeginDrag(2);
        var firstChange = interaction.HandleValueChanged(4);
        var secondChange = interaction.HandleValueChanged(8);
        var completed = interaction.EndDrag();

        Assert.False(firstChange.ShouldApply);
        Assert.False(secondChange.ShouldApply);
        Assert.True(completed.ShouldApply);
        Assert.Equal(8, completed.Value);
        Assert.False(interaction.EndDrag().ShouldApply);
    }

    [Fact]
    public void NonDragUserChange_AppliesForKeyboardAccessibilityAndTapInput()
    {
        var interaction = new NumericRangeSliderInteraction(
            new NumericPropertyRange(0, 10));

        var decision = interaction.HandleValueChanged(7);

        Assert.True(decision.ShouldApply);
        Assert.Equal(7, decision.Value);
    }

    [Fact]
    public void ExplicitIntegerChange_CanResynchronizeWhenSnappingKeepsTheModelUnchanged()
    {
        var interaction = new NumericRangeSliderInteraction(
            new NumericPropertyRange(0, 10));

        var decision = interaction.HandleValueChanged(9.4);
        Assert.True(decision.ShouldApply);

        var unchangedModelValue = 9;
        var displayedValue = interaction.BeginModelSynchronization(unchangedModelValue);
        var synchronizationDecision = interaction.HandleValueChanged(displayedValue);
        interaction.EndModelSynchronization();

        Assert.Equal(9, displayedValue);
        Assert.False(synchronizationDecision.ShouldApply);
    }

    [Fact]
    public void EntryNormalization_UsesTheActualModelValueWithInvariantFormatting()
    {
        var previousCulture = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR");
            var actualClampedModelValue = 10.5f;
            var loadedOutOfRangeModelValue = 25;

            Assert.Equal("10.5", NumericRangeEntryInteraction.Format(actualClampedModelValue));
            Assert.Equal("25", NumericRangeEntryInteraction.Format(loadedOutOfRangeModelValue));
            Assert.Equal("42", NumericRangeEntryInteraction.Format(42u));
        }
        finally
        {
            CultureInfo.CurrentCulture = previousCulture;
        }
    }
}
