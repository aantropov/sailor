using System.Globalization;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Content;

public static class PrefabOverrideApplier
{
    public static string Apply(string sourceYaml, string linkedPrefabYaml)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(sourceYaml);
        ArgumentException.ThrowIfNullOrWhiteSpace(linkedPrefabYaml);

        var sourceStream = new YamlStream();
        sourceStream.Load(new StringReader(sourceYaml));
        var linkedStream = new YamlStream();
        linkedStream.Load(new StringReader(linkedPrefabYaml));
        if (sourceStream.Documents.Count != 1 ||
            linkedStream.Documents.Count != 1 ||
            sourceStream.Documents[0].RootNode is not YamlMappingNode source ||
            linkedStream.Documents[0].RootNode is not YamlMappingNode linked)
        {
            throw new InvalidDataException("Prefab YAML root is invalid.");
        }

        ApplyExpandedGameObjectValues(source, linked);
        ApplyExpandedComponentValues(source, linked);
        ApplyGameObjectOverrides(source, linked);
        ApplyComponentOverrides(source, linked);

        var yaml = new YamlStream(new YamlDocument(source));
        using var writer = new StringWriter(CultureInfo.InvariantCulture);
        yaml.Save(writer, false);
        return writer.ToString();
    }

    static void ApplyExpandedGameObjectValues(
        YamlMappingNode source,
        YamlMappingNode linked)
    {
        if (!TryGetSequence(source, "gameObjects", out var sourceObjects) ||
            !TryGetSequence(linked, "gameObjects", out var linkedObjects))
        {
            return;
        }

        var instanceIds = ReadInstanceIdMap(linked);
        var linkedByInstanceId = IndexByInstanceId(linkedObjects);
        foreach (var sourceObject in sourceObjects.Children
                     .OfType<YamlMappingNode>())
        {
            if (!TryGetScalar(sourceObject, "instanceId", out var sourceId) ||
                !linkedByInstanceId.TryGetValue(
                    ResolveLiveInstanceId(sourceId, instanceIds),
                    out var linkedObject))
            {
                continue;
            }

            foreach (var propertyName in new[]
                     {
                         "name",
                         "scale"
                     })
            {
                var key = new YamlScalarNode(propertyName);
                if (linkedObject.Children.TryGetValue(key, out var value))
                    sourceObject.Children[key] = value;
            }
        }
    }

    static void ApplyExpandedComponentValues(
        YamlMappingNode source,
        YamlMappingNode linked)
    {
        if (!TryGetSequence(source, "components", out var sourceComponents) ||
            !TryGetSequence(linked, "components", out var linkedComponents))
        {
            return;
        }

        var instanceIds = ReadInstanceIdMap(linked);
        var linkedByInstanceId = linkedComponents.Children
            .OfType<YamlMappingNode>()
            .Where(component =>
                TryGetMapping(component, "overrideProperties", out var properties) &&
                TryGetScalar(properties, "instanceId", out _))
            .ToDictionary(
                component => GetScalar(
                    (YamlMappingNode)component.Children[
                        new YamlScalarNode("overrideProperties")],
                    "instanceId"),
                StringComparer.Ordinal);

        foreach (var sourceComponent in sourceComponents.Children
                     .OfType<YamlMappingNode>())
        {
            if (!TryGetMapping(
                    sourceComponent,
                    "overrideProperties",
                    out var sourceProperties) ||
                !TryGetScalar(sourceProperties, "instanceId", out var sourceId) ||
                !linkedByInstanceId.TryGetValue(
                    ResolveLiveInstanceId(sourceId, instanceIds),
                    out var linkedComponent) ||
                !TryGetMapping(
                    linkedComponent,
                    "overrideProperties",
                    out var linkedProperties))
            {
                continue;
            }

            if (TryGetScalar(sourceComponent, "typename", out var sourceType) &&
                TryGetScalar(linkedComponent, "typename", out var linkedType) &&
                !string.Equals(sourceType, linkedType, StringComparison.Ordinal))
            {
                continue;
            }

            CopyComponentProperties(linkedProperties, sourceProperties);
        }
    }

    static void ApplyGameObjectOverrides(
        YamlMappingNode source,
        YamlMappingNode linked)
    {
        if (!TryGetMapping(linked, "gameObjectOverrides", out var overrides) ||
            !TryGetSequence(source, "gameObjects", out var gameObjects))
        {
            return;
        }

        var byInstanceId = gameObjects.Children
            .OfType<YamlMappingNode>()
            .Where(gameObject => TryGetScalar(
                gameObject,
                "instanceId",
                out _))
            .ToDictionary(
                gameObject => GetScalar(gameObject, "instanceId"),
                StringComparer.Ordinal);
        foreach (var entry in overrides.Children)
        {
            var sourceId = (entry.Key as YamlScalarNode)?.Value;
            if (sourceId is null ||
                entry.Value is not YamlMappingNode values ||
                !byInstanceId.TryGetValue(sourceId, out var target))
            {
                continue;
            }

            foreach (var value in values.Children)
            {
                if (IsPrefabRoot(target) &&
                    (value.Key as YamlScalarNode)?.Value is
                        "position" or "rotation")
                {
                    continue;
                }
                target.Children[value.Key] = value.Value;
            }
        }
    }

    static bool IsPrefabRoot(YamlMappingNode gameObject) =>
        TryGetScalar(gameObject, "parentIndex", out var parentIndex) &&
        parentIndex == uint.MaxValue.ToString(CultureInfo.InvariantCulture);

    static void ApplyComponentOverrides(
        YamlMappingNode source,
        YamlMappingNode linked)
    {
        if (!TryGetMapping(linked, "componentOverrides", out var overrides) ||
            !TryGetSequence(source, "components", out var components))
        {
            return;
        }

        var byInstanceId = components.Children
            .OfType<YamlMappingNode>()
            .Where(component =>
                TryGetMapping(component, "overrideProperties", out var properties) &&
                TryGetScalar(properties, "instanceId", out _))
            .ToDictionary(
                component => GetScalar(
                    (YamlMappingNode)component.Children[
                        new YamlScalarNode("overrideProperties")],
                    "instanceId"),
                StringComparer.Ordinal);
        foreach (var entry in overrides.Children)
        {
            var sourceId = (entry.Key as YamlScalarNode)?.Value;
            if (sourceId is null ||
                entry.Value is not YamlMappingNode reflectedOverride ||
                !byInstanceId.TryGetValue(sourceId, out var target) ||
                !TryGetMapping(
                    reflectedOverride,
                    "overrideProperties",
                    out var changedProperties) ||
                !TryGetMapping(
                    target,
                    "overrideProperties",
                    out var targetProperties))
            {
                continue;
            }

            if (TryGetScalar(reflectedOverride, "typename", out var typename))
            {
                target.Children[new YamlScalarNode("typename")] =
                    new YamlScalarNode(typename);
            }
            CopyComponentProperties(changedProperties, targetProperties);
        }
    }

    static void CopyComponentProperties(
        YamlMappingNode source,
        YamlMappingNode target)
    {
        foreach (var property in source.Children)
        {
            if ((property.Key as YamlScalarNode)?.Value is
                "fileId" or "instanceId")
            {
                continue;
            }
            target.Children[property.Key] = property.Value;
        }
    }

    static Dictionary<string, string> ReadInstanceIdMap(
        YamlMappingNode linked)
    {
        if (!TryGetMapping(linked, "instanceIds", out var mapping))
            return new Dictionary<string, string>(StringComparer.Ordinal);

        return mapping.Children
            .Where(entry =>
                entry.Key is YamlScalarNode { Value: not null } &&
                entry.Value is YamlScalarNode { Value: not null })
            .ToDictionary(
                entry => ((YamlScalarNode)entry.Key).Value!,
                entry => ((YamlScalarNode)entry.Value).Value!,
                StringComparer.Ordinal);
    }

    static Dictionary<string, YamlMappingNode> IndexByInstanceId(
        YamlSequenceNode nodes) => nodes.Children
        .OfType<YamlMappingNode>()
        .Where(node => TryGetScalar(node, "instanceId", out _))
        .ToDictionary(
            node => GetScalar(node, "instanceId"),
            StringComparer.Ordinal);

    static string ResolveLiveInstanceId(
        string sourceId,
        IReadOnlyDictionary<string, string> instanceIds)
    {
        if (instanceIds.TryGetValue(sourceId, out var liveId))
            return liveId;

        var separator = sourceId.LastIndexOf('_');
        if (separator >= 0 &&
            instanceIds.TryGetValue(
                sourceId[(separator + 1)..],
                out var liveOwnerId))
        {
            return sourceId[..(separator + 1)] + liveOwnerId;
        }

        return sourceId;
    }

    static bool TryGetMapping(
        YamlMappingNode parent,
        string key,
        out YamlMappingNode mapping)
    {
        if (parent.Children.TryGetValue(
                new YamlScalarNode(key),
                out var value) &&
            value is YamlMappingNode resolved)
        {
            mapping = resolved;
            return true;
        }

        mapping = null!;
        return false;
    }

    static bool TryGetSequence(
        YamlMappingNode parent,
        string key,
        out YamlSequenceNode sequence)
    {
        if (parent.Children.TryGetValue(
                new YamlScalarNode(key),
                out var value) &&
            value is YamlSequenceNode resolved)
        {
            sequence = resolved;
            return true;
        }

        sequence = null!;
        return false;
    }

    static bool TryGetScalar(
        YamlMappingNode parent,
        string key,
        out string value)
    {
        value = string.Empty;
        return parent.Children.TryGetValue(
                new YamlScalarNode(key),
                out var node) &&
            node is YamlScalarNode { Value: not null } scalar &&
            !string.IsNullOrWhiteSpace(value = scalar.Value);
    }

    static string GetScalar(YamlMappingNode parent, string key) =>
        TryGetScalar(parent, key, out var value)
            ? value
            : throw new InvalidDataException(
                $"Prefab field '{key}' is missing.");
}
