using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.ViewModels
{
    public partial class GameObject : ObservableObject, ICloneable
    {
        public List<int> Components = new();

        public object Clone() => new GameObject()
        {
            Name = Name + "(Clone)",
            Position = new Vec4(Position),
            Rotation = new Vec4(Rotation),
            Scale = new Vec4(Scale),
            ParentIndex = ParentIndex,
            InstanceId = InstanceId,
            Components = new List<int>(Components)
        };

        [ObservableProperty]
        string name = string.Empty;

        [ObservableProperty]
        Vec4 position = new Vec4();

        [ObservableProperty]
        Vec4 rotation = new Vec4();

        [ObservableProperty]
        Vec4 scale = new Vec4();

        [ObservableProperty]
        uint parentIndex = uint.MaxValue;

        [ObservableProperty]
        string instanceId = "NullInstanceId";
    }

    public class GameObjectConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(GameObject);

        public object ReadYaml(IParser parser, Type type)
        {
            var deserializer = new DeserializerBuilder()
                .WithNamingConvention(NullNamingConvention.Instance)
                .WithTypeConverter(new Vec4Converter())
                .Build();

            var gameObject = new GameObject();

            // Move to the MappingStart event
            while (parser.Current is not MappingEnd)
            {
                if (parser.Current is Scalar property)
                {
                    switch (property.Value)
                    {
                        case "name":
                            gameObject.Name = deserializer.Deserialize<string>(parser);
                            break;
                        case "position":
                            gameObject.Position = deserializer.Deserialize<Vec4>(parser);
                            break;
                        case "rotation":
                            gameObject.Rotation = deserializer.Deserialize<Vec4>(parser);
                            break;
                        case "scale":
                            gameObject.Scale = deserializer.Deserialize<Vec4>(parser);
                            break;
                        case "parentIndex":
                            gameObject.ParentIndex = deserializer.Deserialize<uint>(parser);
                            break;
                        case "instanceId":
                            gameObject.InstanceId = deserializer.Deserialize<string>(parser);
                            break;
                        case "components":
                            gameObject.Components = deserializer.Deserialize<List<int>>(parser);
                            break;
                        default:
                            //throw new InvalidOperationException($"Unexpected property: {property.Value}");
                            break;
                    }
                }

                parser.MoveNext();
            }

            parser.MoveNext();
            return gameObject;
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var gameObject = (GameObject)value;
            var serializer = new SerializerBuilder()
                .WithNamingConvention(NullNamingConvention.Instance)
                .Build();

            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

            emitter.Emit(new Scalar(null, "name"));
            serializer.Serialize(emitter, gameObject.Name);

            emitter.Emit(new Scalar(null, "position"));
            serializer.Serialize(emitter, gameObject.Position);

            emitter.Emit(new Scalar(null, "rotation"));
            serializer.Serialize(emitter, gameObject.Rotation);

            emitter.Emit(new Scalar(null, "scale"));
            serializer.Serialize(emitter, gameObject.Scale);

            emitter.Emit(new Scalar(null, "parentIndex"));
            serializer.Serialize(emitter, gameObject.ParentIndex);

            emitter.Emit(new Scalar(null, "instanceId"));
            serializer.Serialize(emitter, gameObject.InstanceId);

            emitter.Emit(new Scalar(null, "components"));
            serializer.Serialize(emitter, gameObject.Components);

            emitter.Emit(new MappingEnd());
        }
    }
}
