#nullable enable

using System.Text.Json;
using SailorEditor.Commands;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using YamlDotNet.RepresentationModel;
using ViewModelComponent = SailorEditor.ViewModels.Component;

namespace SailorEditor.Mcp;

internal static class McpSceneCommandFactory
{
    static readonly IReadOnlyDictionary<string, string> s_gameObjectProperties =
        new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
        {
            ["name"] = "name",
            ["mobilityType"] = "mobilityType",
            ["position"] = "position",
            ["rotation"] = "rotation",
            ["scale"] = "scale",
        };

    public static UpdateGameObjectCommand CreateGameObjectUpdate(
        GameObject gameObject,
        IReadOnlyDictionary<string, JsonElement> properties,
        string description)
    {
        var beforeYaml = EditorYaml.SerializeGameObject(gameObject);
        var mapping = McpYamlUtilities.ParseMapping(beforeYaml);
        foreach (var property in properties)
        {
            if (!s_gameObjectProperties.TryGetValue(property.Key, out var canonicalName))
            {
                throw new InvalidDataException(
                    $"GameObject property '{property.Key}' cannot be changed through MCP. " +
                    "Allowed properties: name, mobilityType, position, rotation, scale.");
            }

            if (canonicalName == "mobilityType")
            {
                if (property.Value.ValueKind != JsonValueKind.String ||
                    !GameObjectMobilityPolicy.TryNormalize(
                        property.Value.GetString(),
                        out var mobilityType))
                {
                    throw new InvalidDataException(
                        "GameObject mobilityType must be Static, Stationary, or Dynamic.");
                }

                mapping.Children[new YamlScalarNode(canonicalName)] =
                    new YamlScalarNode(mobilityType);
            }
            else
            {
                mapping.Children[new YamlScalarNode(canonicalName)] =
                    McpYamlUtilities.FromJson(property.Value);
            }
        }

        return new UpdateGameObjectCommand(
            gameObject,
            beforeYaml,
            McpYamlUtilities.Serialize(mapping),
            description);
    }

    public static UpdateComponentCommand CreateComponentUpdate(
        ViewModelComponent component,
        IReadOnlyDictionary<string, JsonElement> properties,
        string description)
    {
        var type = component.Typename ??
            throw new InvalidDataException("The component has no reflected type.");
        var beforeYaml = EditorYaml.SerializeComponent(component);
        var mapping = McpYamlUtilities.ParseMapping(beforeYaml);
        if (!McpYamlUtilities.TryGet(mapping, "overrideProperties", out var overridesNode) ||
            overridesNode is not YamlMappingNode overrides)
        {
            overrides = new YamlMappingNode();
            mapping.Children[new YamlScalarNode("overrideProperties")] = overrides;
        }

        foreach (var property in properties)
        {
            if (string.Equals(property.Key, "instanceId", StringComparison.Ordinal) ||
                string.Equals(property.Key, "fileId", StringComparison.Ordinal) ||
                !type.Properties.ContainsKey(property.Key))
            {
                throw new InvalidDataException(
                    $"Unknown or immutable property '{property.Key}' for component '{type.Name}'.");
            }
            if (type.ReadOnlyProperties.Contains(property.Key))
            {
                throw new InvalidDataException(
                    $"Property '{type.Name}.{property.Key}' is read-only.");
            }

            overrides.Children[new YamlScalarNode(property.Key)] =
                McpYamlUtilities.FromJson(property.Value);
        }

        return new UpdateComponentCommand(
            component,
            beforeYaml,
            McpYamlUtilities.Serialize(mapping),
            description);
    }
}
