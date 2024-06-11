using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEngine;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using YamlDotNet.Core.Tokens;
using Scalar = YamlDotNet.Core.Events.Scalar;

namespace SailorEditor.ViewModels
{
    public partial class Component : ObservableObject, ICloneable
    {
        public Component()
        {
            PropertyChanged += (s, args) =>
            {
                if (args.PropertyName != "IsDirty")
                {
                    IsDirty = true;
                }
            };
        }

        public object Clone() => new Component();

        [ObservableProperty]
        protected bool isDirty = false;

        [ObservableProperty]
        protected string displayName;

        [ObservableProperty]
        ComponentType typename;

        [ObservableProperty]
        ObservableDictionary<string, ObservableObject> overrideProperties = new();
    }

    public class ComponentConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Component);

        public object ReadYaml(IParser parser, Type type)
        {
            var deserializer = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new ObservableDictionaryConverter<string, PropertyBase>())
                .WithTypeConverter(new ComponentTypeConverter())
                .WithTypeConverter(new AssetUIDConverter())
                .WithTypeConverter(new Vec4Converter())
                .WithTypeConverter(new Vec3Converter())
                .WithTypeConverter(new Vec2Converter())
                .Build();

            var component = new Component();

            parser.Consume<MappingStart>();

            while (!parser.TryConsume<MappingEnd>(out var _))
            {
                var propertyName = parser.Consume<Scalar>().Value;

                switch (propertyName)
                {
                    case "typename":
                        component.Typename = deserializer.Deserialize<ComponentType>(parser);
                        break;
                    case "overrideProperties":
                        {
                            parser.Consume<MappingStart>();

                            while (!parser.TryConsume<MappingEnd>(out var _))
                            {
                                if (parser.Current is Scalar scalar)
                                {
                                    string key = scalar.Value;
                                    var propType = component.Typename.Properties[key];

                                    parser.MoveNext();

                                    ObservableObject value = propType switch
                                    {
                                        Vec4Property => deserializer.Deserialize<Vec4>(parser),
                                        Vec3Property => deserializer.Deserialize<Vec3>(parser),
                                        Vec2Property => deserializer.Deserialize<Vec2>(parser),
                                        FileIdProperty => new Observable<AssetUID>(deserializer.Deserialize<string>(parser)),
                                        InstanceIdProperty => new Observable<string>(deserializer.Deserialize<string>(parser)),
                                        FloatProperty => new Observable<float>(deserializer.Deserialize<float>(parser)),
                                        ObjectPtrProperty => new Observable<ObjectPtr>(deserializer.Deserialize<ObjectPtr>(parser)),
                                        EnumProperty => new Observable<string>(deserializer.Deserialize<string>(parser)),
                                        _ => throw new InvalidOperationException($"Unexpected property type: {propType.GetType().Name}")
                                    };

                                    component.OverrideProperties[key] = value;
                                }
                            }

                        }
                        break;
                    default:
                        deserializer.Deserialize<object>(parser);
                        break;
                }
            }

            return component;
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var component = (Component)value;
            var serializer = new SerializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new ObservableDictionaryConverter<string, PropertyBase>())
                .WithTypeConverter(new ComponentTypeConverter())
                .WithTypeConverter(new AssetUIDConverter())
                .WithTypeConverter(new Vec4Converter())
                .WithTypeConverter(new Vec3Converter())
                .WithTypeConverter(new Vec2Converter())
                .Build();

            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

            emitter.Emit(new Scalar(null, "typename"));
            serializer.Serialize(emitter, component.Typename);

            emitter.Emit(new Scalar(null, "overrideProperties"));
            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

            foreach (var kvp in component.OverrideProperties)
            {
                emitter.Emit(new Scalar(null, kvp.Key));
                switch (kvp.Value)
                {
                    case Observable<Vec4> vec4:
                        serializer.Serialize(emitter, vec4.Value);
                        break;
                    case Observable<Vec3> vec3:
                        serializer.Serialize(emitter, vec3.Value);
                        break;
                    case Observable<Vec2> vec2:
                        serializer.Serialize(emitter, vec2.Value);
                        break;
                    case Observable<AssetUID> assetUid:
                        serializer.Serialize(emitter, assetUid.Value);
                        break;
                    case Observable<string> str:
                        serializer.Serialize(emitter, str.Value);
                        break;
                    case Observable<float> floatVal:
                        serializer.Serialize(emitter, floatVal.Value);
                        break;
                    case Observable<ObjectPtr> objPtr:
                        serializer.Serialize(emitter, objPtr.Value);
                        break;
                    default:
                        throw new InvalidOperationException($"Unexpected property type: {kvp.Value.GetType().Name}");
                }
            }

            emitter.Emit(new MappingEnd());

            emitter.Emit(new MappingEnd());
        }
    }
}