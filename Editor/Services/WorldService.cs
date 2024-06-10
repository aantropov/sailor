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

        public WorldService()
        {
            MauiProgram.GetService<EngineService>().OnUpdateCurrentWorldAction += ParseWorld;
        }

        public async void ParseWorld(string yaml)
        {
            var deserializer = new DeserializerBuilder()
            .WithNamingConvention(NullNamingConvention.Instance)
            .WithTypeConverter(new ObservableListConverter<Prefab>(new PrefabConverter()))
            .Build();

            var newWorld = deserializer.Deserialize<World>(yaml);
            Current = newWorld;
        }
    }
}
