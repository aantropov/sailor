using SailorEditor.Services;

namespace SailorEditor.Editor.Tests;

public sealed class GlobalIlluminationBindingInputPolicyTests
{
    [Theory]
    [InlineData("0", 0.0f)]
    [InlineData("0.25", 0.25f)]
    [InlineData("1e2", 100.0f)]
    public void InitialWeight_ParsesTheCurrentInvariantText(
        string text,
        float expected)
    {
        var parsed = GlobalIlluminationBindingInputPolicy.TryParseInitialWeight(
            text,
            out var value);

        Assert.True(parsed);
        Assert.Equal(expected, value);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("-0.01")]
    [InlineData("NaN")]
    [InlineData("Infinity")]
    [InlineData("0,25")]
    [InlineData("invalid")]
    public void InitialWeight_RejectsInvalidCurrentText(string? text)
    {
        var parsed = GlobalIlluminationBindingInputPolicy.TryParseInitialWeight(
            text!,
            out _);

        Assert.False(parsed);
    }
}
