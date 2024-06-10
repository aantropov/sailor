using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using System.ComponentModel;

namespace SailorEditor.ViewModels
{
    public partial class Prefab : ObservableObject, ICloneable
    {
        public object Clone() => new Prefab() { GameObjects = new ObservableList<GameObject>(GameObjects) };

        /*
        [ObservableProperty]
        ObservableList<Component> сomponents = new();
        */

        [ObservableProperty]
        ObservableList<GameObject> gameObjects = new();
    }

    public class PrefabConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Prefab);

        public object ReadYaml(IParser parser, Type type)
        {
            var deserializer = new DeserializerBuilder()
                .WithNamingConvention(NullNamingConvention.Instance)
                .WithTypeConverter(new ObservableListConverter<GameObject>(new GameObjectConverter()))
                .Build();

            parser.MoveNext();

            ObservableList<GameObject> list = null;
            while (parser.Current is not MappingEnd)
            {
                if (parser.Current is Scalar property)
                {
                    if (property.Value == "gameObjects")
                    {
                        list = deserializer.Deserialize<ObservableList<GameObject>>(parser);
                    }

                    parser.MoveNext();
                }
            }

            parser.MoveNext();

            return new Prefab { GameObjects = list };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var prefab = (Prefab)value;
            var serializer = new SerializerBuilder()
                .WithNamingConvention(NullNamingConvention.Instance)
                .WithTypeConverter(new ObservableListConverter<GameObject>(new GameObjectConverter()))
                .Build();

            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));
            serializer.Serialize(emitter, prefab.GameObjects);
            emitter.Emit(new MappingEnd());
        }
    }
}
