using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEngine;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using YamlDotNet.Core.Tokens;
using Scalar = YamlDotNet.Core.Events.Scalar;
using System.Runtime.CompilerServices;
using SailorEditor.Services;
using System.Globalization;
using System;

namespace SailorEditor.ViewModels;

public partial class Component : ObservableObject, ICloneable
{
    public Component()
    {
        PropertyChanged += (s, args) =>
        {
            if (args.PropertyName != nameof(IsDirty))
                IsDirty = true;

            if (args.PropertyName == nameof(OverrideProperties))
                CommitChanges();
        };
    }

    void CommitChanges()
    {
        if (!IsDirty || !isInited)
            return;

        string yamlComponent = string.Empty;

        using (var writer = new StringWriter())
        {
            var serializer = new SerializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new ComponentYamlConverter())
                .Build();

            var yaml = serializer.Serialize(this);
            writer.Write(yaml);

            yamlComponent = writer.ToString();
        }


        MauiProgram.GetService<EngineService>().CommitChanges(InstanceId, yamlComponent);
        IsDirty = false;
    }

    public void Initialize()
    {
        OverrideProperties.CollectionChanged += (a, e) => OnPropertyChanged(nameof(OverrideProperties));
        OverrideProperties.PropertyChanged += (s, args) => OnPropertyChanged(nameof(OverrideProperties));
        OverrideProperties.ValueChanged += (s, args) => OnPropertyChanged(nameof(OverrideProperties));

        IsDirty = false;
        isInited = true;
    }

    public object Clone() => new Component();

    public InstanceId InstanceId { get => OverrideProperties["instanceId"] as Observable<InstanceId>; }

    [YamlIgnore]
    protected bool isInited = false;

    [YamlIgnore]
    protected bool IsDirty
    {
        get => isDirty;
        set => SetProperty(ref isDirty, value);
    }

    [YamlIgnore]
    protected bool isDirty = false;

    [ObservableProperty]
    protected string displayName;

    [ObservableProperty]
    ComponentType typename;

    [ObservableProperty]
    ObservableDictionary<string, ObservableObject> overrideProperties = [];
}

public class ComponentYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(Component);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new ObservableDictionaryConverter<string, PropertyBase>())
            .WithTypeConverter(new ComponentTypeYamlConverter())
            .WithTypeConverter(new FileIdYamlConverter())
            .WithTypeConverter(new RotationYamlConverter())
            .WithTypeConverter(new Vec4YamlConverter())
            .WithTypeConverter(new Vec3YamlConverter())
            .WithTypeConverter(new Vec2YamlConverter())
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
                                    RotationProperty => deserializer.Deserialize<Rotation>(parser),
                                    Vec4Property => deserializer.Deserialize<Vec4>(parser),
                                    Vec3Property => deserializer.Deserialize<Vec3>(parser),
                                    Vec2Property => deserializer.Deserialize<Vec2>(parser),
                                    FileIdProperty => new Observable<FileId>(deserializer.Deserialize<string>(parser)),
                                    InstanceIdProperty => new Observable<InstanceId>(deserializer.Deserialize<string>(parser)),
                                    FloatProperty => new Observable<float>(deserializer.Deserialize<float>(parser)),
                                    ObjectPtrProperty => deserializer.Deserialize<ObjectPtr>(parser),
                                    EnumProperty => new Observable<string>(deserializer.Deserialize<string>(parser)),
                                    _ => throw new InvalidOperationException($"Unexpected property type: {propType.GetType().Name}")
                                };

                                if (propType is ObjectPtrProperty && value == null)
                                {
                                    value = new ObjectPtr();
                                }

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
        var serializer = new SerializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new Vec3YamlConverter())
            .WithTypeConverter(new Vec4YamlConverter())
            .WithTypeConverter(new FileIdYamlConverter())
            .WithTypeConverter(new InstanceIdYamlConverter())
            .WithTypeConverter(new ObservableDictionaryConverter<string, PropertyBase>())
            .Build();
        var component = (Component)value;

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        emitter.Emit(new Scalar(null, "typename"));
        emitter.Emit(new Scalar(null, component.Typename.Name));

        emitter.Emit(new Scalar(null, "overrideProperties"));
        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        var vec3Converter = new Vec3YamlConverter();
        var vec4Converter = new Vec4YamlConverter();
        var vec2Converter = new Vec2YamlConverter();
        var quatConverter = new QuatYamlConverter();
        var rotationConverter = new RotationYamlConverter();
        var fileIdConverter = new FileIdYamlConverter();
        var instanceIdConverter = new InstanceIdYamlConverter();
        var objPtrConverter = new ObjectPtrYamlConverter();

        foreach (var kvp in component.OverrideProperties)
        {
            emitter.Emit(new Scalar(null, kvp.Key));

            switch (kvp.Value)
            {
                case Quat quat:
                    quatConverter.WriteYaml(emitter, quat, typeof(Quat));
                    break;
                case Rotation rot:
                    rotationConverter.WriteYaml(emitter, rot, typeof(Rotation));
                    break;
                case Vec3 vec3:
                    vec3Converter.WriteYaml(emitter, vec3, typeof(Vec3));
                    break;
                case Vec4 vec4:
                    vec4Converter.WriteYaml(emitter, vec4, typeof(Vec4));
                    break;
                case Vec2 vec2:
                    vec2Converter.WriteYaml(emitter, vec2, typeof(Vec2));
                    break;
                case Observable<FileId> assetId:
                    fileIdConverter.WriteYaml(emitter, assetId.Value, typeof(FileId));
                    break;
                case Observable<InstanceId> id:
                    instanceIdConverter.WriteYaml(emitter, id.Value, typeof(InstanceId));
                    break;
                case Observable<string> str:
                    emitter.Emit(new Scalar(null, str.Value));
                    break;
                case Observable<float> floatVal:
                    emitter.Emit(new Scalar(null, floatVal.Value.ToString()));
                    break;
                case ObjectPtr objPtr:
                    objPtrConverter.WriteYaml(emitter, objPtr, typeof(ObjectPtr));
                    break;
                default:
                    throw new InvalidOperationException($"Unexpected property type: {kvp.Value.GetType().Name}");
            }
        }

        emitter.Emit(new MappingEnd());
        emitter.Emit(new MappingEnd());
    }
}