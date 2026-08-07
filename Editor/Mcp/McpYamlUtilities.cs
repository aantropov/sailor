#nullable enable

using System.Globalization;
using System.Text.Json;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Mcp;

internal static class McpYamlUtilities
{
    public static YamlMappingNode ParseMapping(string yaml)
    {
        var stream = new YamlStream();
        using var reader = new StringReader(yaml);
        stream.Load(reader);
        return stream.Documents.FirstOrDefault()?.RootNode as YamlMappingNode ??
            throw new InvalidDataException("A YAML mapping document is required.");
    }

    public static string Serialize(YamlMappingNode mapping)
    {
        var stream = new YamlStream(new YamlDocument(mapping));
        using var writer = new StringWriter(CultureInfo.InvariantCulture);
        stream.Save(writer, false);
        return writer.ToString();
    }

    public static YamlNode FromJson(JsonElement value) => value.ValueKind switch
    {
        JsonValueKind.Object => new YamlMappingNode(value.EnumerateObject()
            .Select(property => new KeyValuePair<YamlNode, YamlNode>(
                new YamlScalarNode(property.Name),
                FromJson(property.Value)))),
        JsonValueKind.Array => new YamlSequenceNode(
            value.EnumerateArray().Select(FromJson)),
        JsonValueKind.String => new YamlScalarNode(value.GetString()),
        JsonValueKind.Number => new YamlScalarNode(value.GetRawText()),
        JsonValueKind.True => new YamlScalarNode("true"),
        JsonValueKind.False => new YamlScalarNode("false"),
        JsonValueKind.Null or JsonValueKind.Undefined => new YamlScalarNode((string?)null),
        _ => throw new InvalidDataException($"Unsupported JSON value kind '{value.ValueKind}'."),
    };

    public static object? ToPlainObject(YamlNode node) => node switch
    {
        YamlMappingNode mapping => mapping.Children.ToDictionary(
            entry => ((YamlScalarNode)entry.Key).Value ?? string.Empty,
            entry => ToPlainObject(entry.Value),
            StringComparer.Ordinal),
        YamlSequenceNode sequence => sequence.Children
            .Select(ToPlainObject)
            .ToArray(),
        YamlScalarNode scalar => ParseScalar(scalar.Value),
        _ => node.ToString(),
    };

    public static IReadOnlyDictionary<string, object?> GetMappingValues(
        YamlMappingNode mapping,
        string key)
    {
        return TryGet(mapping, key, out var node) && node is YamlMappingNode values
            ? (IReadOnlyDictionary<string, object?>)values.Children.ToDictionary(
                entry => ((YamlScalarNode)entry.Key).Value ?? string.Empty,
                entry => ToPlainObject(entry.Value),
                StringComparer.Ordinal)
            : new Dictionary<string, object?>(StringComparer.Ordinal);
    }

    public static bool TryGet(
        YamlMappingNode mapping,
        string key,
        out YamlNode? value)
    {
        return mapping.Children.TryGetValue(
            new YamlScalarNode(key),
            out value);
    }

    static object? ParseScalar(string? value)
    {
        if (value is null || value == "~" ||
            string.Equals(value, "null", StringComparison.OrdinalIgnoreCase))
            return null;
        if (bool.TryParse(value, out var boolean))
            return boolean;
        if (long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var integer))
            return integer;
        // InstanceId values are unsigned 64-bit integers. Keep values outside the
        // signed range as strings so JSON clients never lose bits through a double.
        if (ulong.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out _))
            return value;
        if (double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var number))
            return number;
        return value;
    }
}
