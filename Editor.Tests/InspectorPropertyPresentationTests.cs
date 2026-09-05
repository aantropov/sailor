using SailorEditor.Utility;

namespace SailorEditor.Editor.Tests;

public sealed class InspectorPropertyPresentationTests
{
    [Theory]
    [InlineData("intensity", "Intensity (cd / lux)")]
    [InlineData("indirectLightingIntensity", "GI Intensity")]
    [InlineData("globalIlluminationMode", "GI Mode")]
    [InlineData("radius", "Range (m)")]
    public void FormatPropertyName_UsesLightLabels(
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

    [Theory]
    [InlineData("sunIlluminance", "Sun Illuminance (lux)")]
    [InlineData("cloudScatteringScale", "Cloud Scattering Scale")]
    [InlineData("giIndirectIntensity", "GI Indirect Intensity")]
    public void FormatPropertyName_UsesSkyLightingLabels(
        string propertyName,
        string expected)
    {
        Assert.Equal(
            expected,
            InspectorPropertyPresentation.FormatPropertyName(
                "Sailor::SkyComponent",
                propertyName));
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
