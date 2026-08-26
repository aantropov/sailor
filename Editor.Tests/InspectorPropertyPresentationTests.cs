using SailorEditor.Utility;

namespace SailorEditor.Editor.Tests;

public sealed class InspectorPropertyPresentationTests
{
    [Theory]
    [InlineData("indirectLightingIntensity", "GI Intensity")]
    [InlineData("globalIlluminationMode", "GI Mode")]
    [InlineData("intensity", "intensity")]
    public void FormatPropertyName_UsesLightGiLabels(
        string propertyName,
        string expected)
    {
        Assert.Equal(
            expected,
            InspectorPropertyPresentation.FormatPropertyName(
                "Sailor::LightComponent",
                propertyName));
    }

    [Theory]
    [InlineData("Realtime", "Realtime")]
    [InlineData("RealtimeAndBaked", "Realtime + Baked")]
    [InlineData("BakedOnly", "Baked Indirect")]
    public void FormatEnumValue_UsesFriendlyLightGiModeLabels(
        string value,
        string expected)
    {
        Assert.Equal(
            expected,
            InspectorPropertyPresentation.FormatEnumValue(
                "Sailor::LightComponent",
                "globalIlluminationMode",
                value));
    }

    [Fact]
    public void FormatPropertyName_UsesSkyGiIndirectIntensityLabel()
    {
        Assert.Equal(
            "GI Indirect Intensity",
            InspectorPropertyPresentation.FormatPropertyName(
                "Sailor::SkyComponent",
                "giIndirectIntensity"));
    }

    [Fact]
    public void Presentation_LeavesUnrelatedPropertiesAndEnumsUntouched()
    {
        Assert.Equal(
            "globalIlluminationMode",
            InspectorPropertyPresentation.FormatPropertyName(
                "Sandbox::LightComponent",
                "globalIlluminationMode"));
        Assert.Equal(
            "RealtimeAndBaked",
            InspectorPropertyPresentation.FormatEnumValue(
                "Sailor::SkyComponent",
                "mode",
                "RealtimeAndBaked"));
    }
}
