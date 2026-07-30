#nullable enable

using System.Globalization;
using SailorEngine;

namespace SailorEditor.Utility;

public readonly record struct NumericRangeSliderDecision(
    bool ShouldPreview,
    bool ShouldApply,
    double Value)
{
    public static NumericRangeSliderDecision None { get; } = new(false, false, 0);

    public static NumericRangeSliderDecision Preview(double value) => new(true, false, value);

    public static NumericRangeSliderDecision Apply(double value) => new(true, true, value);
}

public sealed class NumericRangeSliderInteraction
{
    readonly NumericPropertyRange range;
    int modelSynchronizationDepth;
    bool isDragging;
    double pendingDragValue;

    public NumericRangeSliderInteraction(NumericPropertyRange range)
    {
        this.range = range;
    }

    public double BeginModelSynchronization(double modelValue)
    {
        modelSynchronizationDepth++;
        var displayedValue = range.Clamp(modelValue);
        if (isDragging)
            pendingDragValue = displayedValue;

        return displayedValue;
    }

    public void EndModelSynchronization()
    {
        if (modelSynchronizationDepth == 0)
            throw new InvalidOperationException("No numeric range model synchronization is active.");

        modelSynchronizationDepth--;
    }

    public void BeginDrag(double displayedValue)
    {
        isDragging = true;
        pendingDragValue = range.Clamp(displayedValue);
    }

    public NumericRangeSliderDecision HandleValueChanged(double displayedValue)
    {
        if (modelSynchronizationDepth != 0)
            return NumericRangeSliderDecision.None;

        var clampedValue = range.Clamp(displayedValue);
        if (isDragging)
        {
            pendingDragValue = clampedValue;
            return NumericRangeSliderDecision.Preview(clampedValue);
        }

        return NumericRangeSliderDecision.Apply(clampedValue);
    }

    public NumericRangeSliderDecision EndDrag()
    {
        if (!isDragging)
            return NumericRangeSliderDecision.None;

        isDragging = false;
        return NumericRangeSliderDecision.Apply(pendingDragValue);
    }
}

public static class NumericRangeEntryInteraction
{
    public static string Format(float value) => value.ToString(CultureInfo.InvariantCulture);

    public static string Format(int value) => value.ToString(CultureInfo.InvariantCulture);

    public static string Format(uint value) => value.ToString(CultureInfo.InvariantCulture);
}
