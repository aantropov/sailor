using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Utility;
using SailorEngine;

namespace SailorEditor.Services
{
    public partial class WorldService : ObservableObject
    {
        public World Current { get; private set; } = new World();

        [ObservableProperty]
        ObservableList<ObservableList<GameObject>> gameObjects = new();

        public event Action<World> OnUpdateWorldAction = delegate { };

        public WorldService()
        {
            MauiProgram.GetService<EngineService>().OnUpdateCurrentWorldAction += ParseWorld;
        }

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

        public async void ParseWorld(string yaml)
        {
            var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new ObservableListConverter<Prefab>(
                new IYamlTypeConverter[]
                {
                    new RotationYamlConverter(),
                    new Vec4YamlConverter(),
                    new Vec3YamlConverter(),
                    new Vec2YamlConverter(),
                    new ComponentTypeYamlConverter(),
                    new ViewModels.ComponentYamlConverter()
                }))
            .IncludeNonPublicProperties()
            .Build();

            var newWorld = deserializer.Deserialize<World>(yaml);
            Current = newWorld;

            GameObjects.Clear();

            int prefabIndex = 0;
            foreach (var prefab in Current.Prefabs)
            {
                foreach (var go in prefab.GameObjects)
                {
                    go.PrefabIndex = prefabIndex;

                    gameObjectsDict[go.InstanceId] = go;

                    foreach (var i in go.ComponentIndices)
                    {
                        var component = prefab.Components[i];
                        componentsDict[component.InstanceId] = component;
                        component.DisplayName = $"{go.DisplayName} ({component.Typename.Name})";

                        component.Refresh();
                    }

                    go.Refresh();
                }

                GameObjects.Add([.. prefab.GameObjects]);

                prefabIndex++;
            }

            OnUpdateWorldAction?.Invoke(Current);
        }

        Dictionary<InstanceId, Component> componentsDict = new();
        Dictionary<InstanceId, GameObject> gameObjectsDict = new();
    }
}
