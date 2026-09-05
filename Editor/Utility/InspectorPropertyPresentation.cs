namespace SailorEditor.Utility;

public static class InspectorPropertyPresentation
{
    const string LightComponentTypeName = "Sailor::LightComponent";
    const string SkyComponentTypeName = "Sailor::SkyComponent";
    const string GlobalIlluminationModePropertyName = "globalIlluminationMode";

    public static string FormatPropertyName(
        string componentTypeName,
        string propertyName)
    {
        if (componentTypeName == SkyComponentTypeName)
        {
            return propertyName switch
            {
                "sunIlluminance" => "Sun Illuminance (lux)",
                "cloudScatteringScale" => "Cloud Scattering Scale",
                "giIndirectIntensity" => "GI Indirect Intensity",
                _ => propertyName
            };
        }

        if (componentTypeName != LightComponentTypeName)
        {
            return propertyName;
        }

        return propertyName switch
        {
            "intensity" => "Intensity (cd / lux)",
            "indirectLightingIntensity" => "GI Intensity",
            GlobalIlluminationModePropertyName => "GI Mode",
            "radius" => "Range (m)",
            _ => propertyName
        };
    }

    public static string FormatEnumValue(
        string componentTypeName,
        string propertyName,
        string value)
    {
        if (componentTypeName != LightComponentTypeName ||
            propertyName != GlobalIlluminationModePropertyName)
        {
            return value;
        }

        return value switch
        {
            "Realtime" => "Realtime",
            "RealtimeAndBaked" => "Realtime + Baked",
            "BakedOnly" => "Baked Indirect",
            _ => value
        };
    }
}
