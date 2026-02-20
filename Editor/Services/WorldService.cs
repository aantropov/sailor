using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Utility;
using SailorEngine;
using YamlDotNet.Core.Tokens;

namespace SailorEditor.Services
{
    public partial class WorldService : ObservableObject
    {
        class WorldCache
        {
            public Dictionary<InstanceId, Component> Components { get; } = new();
            public Dictionary<InstanceId, GameObject> GameObjects { get; } = new();
        }

        public World Current { get; private set; } = new World();

        [ObservableProperty]
        ObservableList<ObservableList<GameObject>> gameObjects = new();

        public event Action<World> OnUpdateWorldAction = delegate { };

        public WorldService()
        {
            MauiProgram.GetService<EngineService>().OnUpdateCurrentWorldAction += PopulateWorld;
        }

        static string GetWorldKey(World world) => string.IsNullOrEmpty(world?.Name) ? "__default_world__" : world.Name;

        public Component GetComponent(InstanceId instanceId) => componentsDict[instanceId];
        public GameObject GetGameObject(InstanceId instanceId) => gameObjectsDict[instanceId];

        public List<Component> GetComponents(GameObject gameObject)
        {
            List<Component> res = new();
            foreach (var componentIndex in gameObject.ComponentIndices)
            {
                res.Add(Current.Prefabs[gameObject.PrefabIndex].Components[componentIndex]);
            }

            return res;
        }

        public void PopulateWorld(string yaml)
        {
            var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new WorldYamlConverter())
            .Build();

            var world = deserializer.Deserialize<World>(yaml);
            if (world == null)
                return;

            GameObjects.Clear();
            Current.Prefabs.Clear();

            Current.Name = world.Name;

            var worldKey = GetWorldKey(world);
            if (!worldCaches.TryGetValue(worldKey, out var cache))
            {
                cache = new WorldCache();
                worldCaches[worldKey] = cache;
            }

            // Rebuild only this world's lookup caches.
            cache.Components.Clear();
            cache.GameObjects.Clear();
            currentCache = cache;

            int prefabIndex = 0;
            foreach (var prefab in world.Prefabs)
            {
                var newPrefab = new Prefab();
                foreach (var go in prefab.GameObjects)
                {
                    var gameObject = go;
                    gameObjectsDict[go.InstanceId] = gameObject;

                    gameObject.PrefabIndex = prefabIndex;
                    foreach (var i in go.ComponentIndices)
                    {
                        var component = prefab.Components[i];
                        componentsDict[component.InstanceId] = component;
                        component.DisplayName = $"{go.DisplayName} ({component.Typename.Name})";

                        component.Initialize();
                        newPrefab.Components.Add(component);
                    }

                    gameObject.Initialize();
                    newPrefab.GameObjects.Add(gameObject);
                }

                Current.Prefabs.Add(newPrefab);
                GameObjects.Add([.. newPrefab.GameObjects]);

                prefabIndex++;
            }

            OnUpdateWorldAction?.Invoke(Current);
        }

        public string SerializeCurrentWorld()
        {
            string yamlWorld = string.Empty;

            using (var writer = new StringWriter())
            {
                var serializer = SerializationUtils.CreateSerializerBuilder()
                .WithTypeConverter(new WorldYamlConverter())
                .Build();

                var yaml = serializer.Serialize(Current);
                writer.Write(yaml);

                yamlWorld = writer.ToString();
            }

            return yamlWorld;
        }

        readonly Dictionary<string, WorldCache> worldCaches = new();
        WorldCache currentCache = new();

        Dictionary<InstanceId, Component> componentsDict => currentCache.Components;
        Dictionary<InstanceId, GameObject> gameObjectsDict => currentCache.GameObjects;
    }
}
