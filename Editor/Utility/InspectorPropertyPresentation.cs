namespace SailorEditor.Utility;

public static class InspectorPropertyPresentation
{
    const string LightComponentTypeName = "Sailor::LightComponent";
    const string GlobalIlluminationModePropertyName = "globalIlluminationMode";

    public static string FormatPropertyName(
        string componentTypeName,
        string propertyName)
    {
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
            "BakedOnly" => "Baked Only",
            _ => value
        };
    }
}
