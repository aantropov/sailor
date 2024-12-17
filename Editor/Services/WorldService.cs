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
        public World Current { get; private set; } = new World();

        [ObservableProperty]
        ObservableList<ObservableList<GameObject>> gameObjects = new();

        public event Action<World> OnUpdateWorldAction = delegate { };

        public WorldService()
        {
            MauiProgram.GetService<EngineService>().OnUpdateCurrentWorldAction += PopulateWorld;
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

        public async void PopulateWorld(string yaml)
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

            var world = deserializer.Deserialize<World>(yaml);

            GameObjects.Clear();
            Current.Prefabs.Clear();

            int prefabIndex = 0;
            foreach (var prefab in world.Prefabs)
            {
                var newPrefab = new Prefab();
                foreach (var go in prefab.GameObjects)
                {
                    GameObject gameObject = null;

                    if (!gameObjectsDict.TryGetValue(go.InstanceId, out gameObject))
                        gameObject = gameObjectsDict[go.InstanceId] = go;

                    gameObject.PrefabIndex = prefabIndex;
                    foreach (var i in go.ComponentIndices)
                    {
                        Component component;

                        if (!componentsDict.TryGetValue(prefab.Components[i].InstanceId, out component))
                        {
                            component = componentsDict[prefab.Components[i].InstanceId] = prefab.Components[i];
                            component.DisplayName = $"{go.DisplayName} ({component.Typename.Name})";
                        }
                        else
                        {
                            component.Typename = prefab.Components[i].Typename;

                            foreach (var key in component.OverrideProperties.Keys)
                                if (!prefab.Components[i].OverrideProperties.ContainsKey(key))
                                    component.OverrideProperties.Remove(key);

                            foreach (var prop in prefab.Components[i].OverrideProperties)
                            {
                                if (!component.OverrideProperties.ContainsKey(prop.Key))
                                    component.OverrideProperties[prop.Key] = prop.Value;
                                else
                                    component.OverrideProperties[prop.Key].CopyPropertiesFrom(prop.Value);
                            }
                        }

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
                var commonConverters = new List<IYamlTypeConverter> {
                    new RotationYamlConverter(),
                    new Vec4YamlConverter(),
                    new Vec3YamlConverter(),
                    new Vec2YamlConverter(),
                    new InstanceIdYamlConverter(),
                    new FileIdYamlConverter(),
                    new ComponentTypeYamlConverter(),
                    new ViewModels.ComponentYamlConverter()
                    };

                var serializerBuilder = new SerializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new ObservableListConverter<Prefab>(
                    new IYamlTypeConverter[]{
                        new ObservableListConverter<GameObject>(commonConverters.ToArray()),
                        new ObservableListConverter<Component>(commonConverters.ToArray())
                    }))
                .IncludeNonPublicProperties();

                commonConverters.ForEach((el) => serializerBuilder.WithTypeConverter(el));

                var serializer = serializerBuilder.Build();

                var yaml = serializer.Serialize(Current);
                writer.Write(yaml);

                yamlWorld = writer.ToString();
            }

            return yamlWorld;
        }

        Dictionary<InstanceId, Component> componentsDict = new();
        Dictionary<InstanceId, GameObject> gameObjectsDict = new();
    }
}
