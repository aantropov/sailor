using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
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
using SailorEditor.Workflow;

namespace SailorEditor.ViewModels;

public partial class Component : ObservableObject, ICloneable, IInspectorEditable
{
    readonly InspectorAutoCommitController _autoCommit = new(
        propertyName => propertyName == nameof(IsDirty),
        propertyName => false);

    public Component()
    {
        PropertyChanged += (s, args) =>
        {
            var decision = _autoCommit.OnPropertyChanged(args.PropertyName);
            if (decision.MarkDirty)
                IsDirty = true;
        };
    }

    public bool CommitInspectorChanges()
    {
        if (!_autoCommit.ShouldCommitPendingChanges(IsDirty) || !isInited)
            return false;

        var yamlComponent = EditorYaml.SerializeComponent(this);
        var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
        var contextProvider = MauiProgram.GetService<IActionContextProvider>();
        var result = dispatcher.DispatchAsync(
            new UpdateComponentCommand(this, _lastCommittedYaml ?? yamlComponent, yamlComponent, $"Edit {Typename?.Name}"),
            contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(CommitInspectorChanges))))
            .GetAwaiter()
            .GetResult();

        if (result.Succeeded)
        {
            _lastCommittedYaml = yamlComponent;
            IsDirty = false;
            return true;
        }

        return false;
    }

    public bool HasPendingInspectorChanges => IsDirty;

    public void Initialize()
    {
        OverrideProperties.CollectionChanged += (a, e) => OnPropertyChanged(nameof(OverrideProperties));
        OverrideProperties.PropertyChanged += (s, args) => OnPropertyChanged(nameof(OverrideProperties));
        OverrideProperties.ValueChanged += (s, args) => OnPropertyChanged(nameof(OverrideProperties));

        IsDirty = false;
        _lastCommittedYaml = EditorYaml.SerializeComponent(this);
        isInited = true;
        _autoCommit.MarkInitialized();
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

    [YamlIgnore]
    string? _lastCommittedYaml;

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
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new ObservableDictionaryConverter<string, PropertyBase>())
            .WithTypeConverter(new ComponentTypeYamlConverter())
            .WithTypeConverter(new ComponentYamlConverter())
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

                                parser.MoveNext();
                                if (component.Typename?.Properties == null ||
                                    !component.Typename.Properties.TryGetValue(key, out var propType))
                                {
                                    deserializer.Deserialize<object>(parser);
                                    continue;
                                }

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
        var serializer = SerializationUtils.CreateSerializerBuilder()            
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
