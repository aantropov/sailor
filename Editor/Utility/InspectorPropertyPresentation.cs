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
        if (componentTypeName == SkyComponentTypeName &&
            propertyName == "giIndirectIntensity")
        {
            return "GI Indirect Intensity";
        }

        if (componentTypeName != LightComponentTypeName)
        {
            return propertyName;
        }

        return propertyName switch
        {
            "indirectLightingIntensity" => "GI Intensity",
            GlobalIlluminationModePropertyName => "GI Mode",
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
