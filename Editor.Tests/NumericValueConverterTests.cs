using System.Globalization;
using SailorEditor.Controls;
using SailorEditor.Utility;

namespace SailorEditor.Editor.Tests;

public sealed class NumericValueConverterTests
{
    [Theory]
    [InlineData("")]
    [InlineData("-")]
    [InlineData("not-a-number")]
    public void IntConverter_InvalidTransientTextDoesNotReplaceTheBoundValue(string text)
    {
        var result = new IntValueConverter().ConvertBack(text, typeof(int), null!, CultureInfo.InvariantCulture);

        Assert.Same(BindableProperty.UnsetValue, result);
    }

    [Theory]
    [InlineData("")]
    [InlineData("-1")]
    [InlineData("not-a-number")]
    public void UIntConverter_InvalidTransientTextDoesNotReplaceTheBoundValue(string text)
    {
        var result = new UIntValueConverter().ConvertBack(text, typeof(uint), null!, CultureInfo.InvariantCulture);

        Assert.Same(BindableProperty.UnsetValue, result);
    }

    [Theory]
    [InlineData("")]
    [InlineData("-")]
    [InlineData("NaN")]
    [InlineData("Infinity")]
    [InlineData("not-a-number")]
    public void FloatConverter_InvalidTransientTextDoesNotReplaceTheBoundValue(string text)
    {
        var result = new FloatValueConverter().ConvertBack(text, typeof(float), null!, CultureInfo.InvariantCulture);

        Assert.Same(BindableProperty.UnsetValue, result);
    }

    [Fact]
    public void NumericConverters_ParseInvariantValuesThroughConvertBack()
    {
        Assert.Equal(-42, new IntValueConverter().ConvertBack("-42", typeof(int), null!, CultureInfo.GetCultureInfo("fr-FR")));
        Assert.Equal(42u, new UIntValueConverter().ConvertBack("42", typeof(uint), null!, CultureInfo.GetCultureInfo("fr-FR")));
        Assert.Equal(3.125f, new FloatValueConverter().ConvertBack("3.125", typeof(float), null!, CultureInfo.GetCultureInfo("fr-FR")));
    }

    [Fact]
    public async Task FloatConverter_BindsMixedTransformAxesThroughObjectPropertiesWithoutReplacingUntouchedAxes()
    {
        var x = new object?[] { -2f, 2f };
        var y = new object?[] { 1f, 1f };
        var z = new object?[] { 4f, 8f };
        var vector = new InspectorAggregateField("Position", typeof(SailorEditor.Vec4), [])
        {
            Children = [Axis("X", x), Axis("Y", y), Axis("Z", z)]
        };
        var binding = new InspectorAggregateVectorBinding(vector);
        var converter = new FloatValueConverter();

        Assert.Equal("*", converter.Convert(binding.X, typeof(string), null!, CultureInfo.InvariantCulture));
        Assert.Equal("1", converter.Convert(binding.Y, typeof(string), null!, CultureInfo.InvariantCulture));
        Assert.Equal("*", converter.Convert(binding.Z, typeof(string), null!, CultureInfo.InvariantCulture));
        Assert.False(vector.HasPendingInspectorChanges);
        Assert.Same(BindableProperty.UnsetValue, converter.ConvertBack("*", typeof(object), null!, CultureInfo.InvariantCulture));

        var parsed = converter.ConvertBack("2.6", typeof(object), null!, CultureInfo.GetCultureInfo("fr-FR"));
        typeof(InspectorAggregateVectorBinding).GetProperty(nameof(InspectorAggregateVectorBinding.Y))!.SetValue(binding, parsed);
        Assert.True(await vector.CommitInspectorChangesAsync());

        Assert.Equal(new object?[] { -2f, 2f }, x);
        Assert.Equal(new object?[] { 2.6f, 2.6f }, y);
        Assert.Equal(new object?[] { 4f, 8f }, z);
        Assert.Equal("2.6", converter.Convert(binding.Y, typeof(string), null!, CultureInfo.InvariantCulture));
    }

    static InspectorAggregateField Axis(string name, object?[] values) => new(name, typeof(float),
        values.Select((_, index) => new InspectorAggregateValueTarget(() => values[index], (value, _) =>
        {
            values[index] = value;
            return Task.FromResult(true);
        })).ToArray());
}
