using SailorEditor.Utility;

namespace SailorEditor.Editor.Tests;

public sealed class GameObjectMobilityPolicyTests
{
    [Theory]
    [InlineData("Static", "Static", true)]
    [InlineData("Static", "Stationary", true)]
    [InlineData("Static", "Dynamic", true)]
    [InlineData("Stationary", "Static", false)]
    [InlineData("Stationary", "Dynamic", true)]
    [InlineData("Dynamic", "Stationary", false)]
    [InlineData("Dynamic", "Dynamic", true)]
    public void Hierarchy_AllowsSameOrMoreMovableChildren(
        string parent,
        string child,
        bool expected)
    {
        Assert.Equal(
            expected,
            GameObjectMobilityPolicy.IsSameOrMoreMovable(parent, child));
    }

    [Theory]
    [InlineData(null, "Stationary")]
    [InlineData("", "Stationary")]
    [InlineData("stationary", "Stationary")]
    [InlineData("Dynamic", "Dynamic")]
    [InlineData("invalid", "Stationary")]
    public void Normalize_PreservesKnownValuesAndDefaultsLegacyData(
        string? value,
        string expected)
    {
        Assert.Equal(expected, GameObjectMobilityPolicy.Normalize(value));
    }

    [Fact]
    public void HasMobilityChange_TreatsMissingLegacyValueAsStationary()
    {
        const string legacy = "name: Legacy\n";
        const string unchanged = "name: Current\nmobilityType: Stationary\n";
        const string changed = "name: Current\nmobilityType: Dynamic\n";

        Assert.False(GameObjectMobilityPolicy.HasMobilityChange(legacy, unchanged));
        Assert.True(GameObjectMobilityPolicy.HasMobilityChange(legacy, changed));
    }

    [Theory]
    [InlineData("static", true, "Static")]
    [InlineData("Stationary", true, "Stationary")]
    [InlineData("dynamic", true, "Dynamic")]
    [InlineData("movable", false, "Stationary")]
    public void TryNormalize_RejectsUnknownApiValues(
        string value,
        bool expectedSuccess,
        string expectedValue)
    {
        Assert.Equal(
            expectedSuccess,
            GameObjectMobilityPolicy.TryNormalize(value, out var normalized));
        Assert.Equal(expectedValue, normalized);
    }
}
