using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Utility;

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

        public async void ParseWorld(string yaml)
        {
            var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new ObservableListConverter<Prefab>(
                new IYamlTypeConverter[]
                {
                    new Vec4Converter(),
                    new Vec3Converter(),
                    new Vec2Converter(),
                    new ComponentTypeConverter(),
                    new ViewModels.ComponentConverter()
                }))
            .IncludeNonPublicProperties()
            .Build();

            var newWorld = deserializer.Deserialize<World>(yaml);
            Current = newWorld;

            GameObjects.Clear();
            foreach (var prefab in Current.Prefabs)
            {
                GameObjects.Add([.. prefab.GameObjects]);
            }

            OnUpdateWorldAction?.Invoke(Current);
        }
    }
}
