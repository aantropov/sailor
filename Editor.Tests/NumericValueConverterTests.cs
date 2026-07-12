using System.Globalization;
using SailorEditor.Controls;

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
}
