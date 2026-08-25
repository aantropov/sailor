using YamlDotNet.RepresentationModel;

namespace SailorEditor.Utility;

public static class GameObjectMobilityPolicy
{
    public const string Static = "Static";
    public const string Stationary = "Stationary";
    public const string Dynamic = "Dynamic";

    static readonly IList<string> s_values = Array.AsReadOnly(
        new[] { Static, Stationary, Dynamic });

    public static IList<string> Values => s_values;

    public static string Normalize(string? value)
    {
        return TryNormalize(value, out var normalized)
            ? normalized
            : Stationary;
    }

    public static bool TryNormalize(string? value, out string normalized)
    {
        foreach (var candidate in s_values)
        {
            if (string.Equals(candidate, value, StringComparison.OrdinalIgnoreCase))
            {
                normalized = candidate;
                return true;
            }
        }

        normalized = Stationary;
        return false;
    }

    public static bool IsSameOrMoreMovable(
        string? parentMobility,
        string? childMobility)
        => Rank(Normalize(childMobility)) >= Rank(Normalize(parentMobility));

    public static bool HasMobilityChange(string beforeYaml, string afterYaml)
    {
        try
        {
            return !string.Equals(
                ReadMobility(beforeYaml),
                ReadMobility(afterYaml),
                StringComparison.Ordinal);
        }
        catch
        {
            return true;
        }
    }

    static string ReadMobility(string yaml)
    {
        var stream = new YamlStream();
        stream.Load(new StringReader(yaml));
        if (stream.Documents.Count == 0 ||
            stream.Documents[0].RootNode is not YamlMappingNode mapping)
        {
            return Stationary;
        }

        foreach (var property in mapping.Children)
        {
            if (property.Key is YamlScalarNode key &&
                string.Equals(key.Value, "mobilityType", StringComparison.Ordinal) &&
                property.Value is YamlScalarNode value)
            {
                return Normalize(value.Value);
            }
        }

        return Stationary;
    }

    static int Rank(string mobility) => mobility switch
    {
        Static => 0,
        Stationary => 1,
        Dynamic => 2,
        _ => 1
    };
}
