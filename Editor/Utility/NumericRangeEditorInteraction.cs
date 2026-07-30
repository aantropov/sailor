#nullable enable

using System.Globalization;
using SailorEngine;

namespace SailorEditor.Utility;

public readonly record struct NumericRangeSliderDecision(bool ShouldApply, double Value)
{
    public static NumericRangeSliderDecision None { get; } = new(false, 0);

    public static NumericRangeSliderDecision Apply(double value) => new(true, value);
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
        return range.Clamp(modelValue);
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
            return NumericRangeSliderDecision.None;
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
