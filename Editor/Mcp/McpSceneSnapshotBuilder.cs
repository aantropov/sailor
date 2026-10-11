#nullable enable

using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using YamlDotNet.RepresentationModel;
using ViewModelComponent = SailorEditor.ViewModels.Component;

namespace SailorEditor.Mcp;

internal sealed class McpSceneSnapshotBuilder
{
    readonly WorldService _world;
    readonly EngineService _engine;

    public McpSceneSnapshotBuilder(
        WorldService world,
        EngineService engine)
    {
        _world = world;
        _engine = engine;
    }

    public McpSceneHierarchySnapshot Build(bool includeYaml)
    {
        var world = _world.Current;
        var allObjects = world.Prefabs
            .SelectMany(prefab => prefab.GameObjects)
            .Where(gameObject =>
                gameObject.InstanceId is not null &&
                !gameObject.InstanceId.IsEmpty())
            .GroupBy(gameObject => gameObject.InstanceId.Value, StringComparer.Ordinal)
            .Select(group => group.First())
            .ToArray();
        var builders = allObjects.ToDictionary(
            gameObject => gameObject.InstanceId.Value,
            gameObject => new GameObjectBuilder(
                gameObject,
                _world.ResolveParentInstanceId(gameObject)?.Value,
                ResolvePrefabFileId(world, gameObject),
                BuildComponents(gameObject, includeYaml)),
            StringComparer.Ordinal);

        var roots = new List<GameObjectBuilder>();
        foreach (var builder in builders.Values)
        {
            if (!string.IsNullOrWhiteSpace(builder.ParentInstanceId) &&
                builders.TryGetValue(builder.ParentInstanceId, out var parent))
            {
                parent.Children.Add(builder);
            }
            else
            {
                roots.Add(builder);
            }
        }

        return new McpSceneHierarchySnapshot(
            world.Name,
            _world.CurrentWorldAsset?.FileId?.Value,
            _world.WorkspaceEpoch,
            roots
                .OrderBy(root => root.GameObject.Name, StringComparer.Ordinal)
                .ThenBy(root => root.GameObject.InstanceId.Value, StringComparer.Ordinal)
                .Select(ToSnapshot)
                .ToArray(),
            includeYaml ? _world.SerializeCurrentWorld() : null);
    }

    public McpGameObjectSnapshot? BuildObject(string instanceId)
    {
        if (!_world.TryGetGameObject(new InstanceId(instanceId), out var gameObject))
            return null;

        return ToSnapshot(new GameObjectBuilder(
            gameObject,
            _world.ResolveParentInstanceId(gameObject)?.Value,
            ResolvePrefabFileId(_world.Current, gameObject),
            BuildComponents(gameObject, includeYaml: true)));
    }

    public IReadOnlyList<McpComponentTypeSchema> BuildComponentSchemas() =>
        new McpComponentSchemaBuilder(_engine.EngineTypes).Build();

    IReadOnlyList<McpComponentSnapshot> BuildComponents(
        GameObject gameObject,
        bool includeYaml) =>
        _world.GetComponents(gameObject)
            .Where(component =>
                component.InstanceId is not null &&
                !component.InstanceId.IsEmpty())
            .Select(component => BuildComponent(component, includeYaml))
            .ToArray();

    static McpComponentSnapshot BuildComponent(
        ViewModelComponent component,
        bool includeYaml)
    {
        var yaml = EditorYaml.SerializeComponent(component);
        var mapping = McpYamlUtilities.ParseMapping(yaml);
        return new McpComponentSnapshot(
            component.InstanceId.Value,
            component.Typename?.Name ?? string.Empty,
            McpYamlUtilities.GetMappingValues(mapping, "overrideProperties"),
            includeYaml ? yaml : null);
    }

    static McpGameObjectSnapshot ToSnapshot(GameObjectBuilder builder)
    {
        var yaml = EditorYaml.SerializeGameObject(builder.GameObject);
        var mapping = McpYamlUtilities.ParseMapping(yaml);
        var properties = mapping.Children
            .Where(entry =>
                !string.Equals(
                    ((YamlScalarNode)entry.Key).Value,
                    "components",
                    StringComparison.Ordinal))
            .ToDictionary(
                entry => ((YamlScalarNode)entry.Key).Value ?? string.Empty,
                entry => McpYamlUtilities.ToPlainObject(entry.Value),
                StringComparer.Ordinal);

        return new McpGameObjectSnapshot(
            builder.GameObject.InstanceId.Value,
            builder.GameObject.Name,
            builder.ParentInstanceId,
            builder.PrefabFileId,
            properties,
            builder.Components,
            builder.Children
                .OrderBy(child => child.GameObject.Name, StringComparer.Ordinal)
                .ThenBy(child => child.GameObject.InstanceId.Value, StringComparer.Ordinal)
                .Select(ToSnapshot)
                .ToArray());
    }

    static string? ResolvePrefabFileId(World world, GameObject gameObject)
    {
        if (gameObject.PrefabIndex < 0 || gameObject.PrefabIndex >= world.Prefabs.Count)
            return null;
        var fileId = world.Prefabs[gameObject.PrefabIndex].FileId;
        return fileId is null || fileId.IsEmpty() ? null : fileId.Value;
    }

    sealed class GameObjectBuilder
    {
        public GameObjectBuilder(
            GameObject gameObject,
            string? parentInstanceId,
            string? prefabFileId,
            IReadOnlyList<McpComponentSnapshot> components)
        {
            GameObject = gameObject;
            ParentInstanceId = parentInstanceId;
            PrefabFileId = prefabFileId;
            Components = components;
        }

        public GameObject GameObject { get; }
        public string? ParentInstanceId { get; }
        public string? PrefabFileId { get; }
        public IReadOnlyList<McpComponentSnapshot> Components { get; }
        public List<GameObjectBuilder> Children { get; } = [];
    }
}
