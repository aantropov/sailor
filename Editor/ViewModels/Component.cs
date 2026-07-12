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

    [YamlIgnore]
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

    [YamlIgnore]
    public Dictionary<string, object?> PreservedReadOnlyProperties { get; } = new(StringComparer.Ordinal);

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

        var serializer = SerializationUtils.CreateSerializerBuilder().Build();
        var document = deserializer.Deserialize<EditorComponentYamlContract>(parser) ??
            throw new YamlException("A component YAML document is required.");
        if (string.IsNullOrWhiteSpace(document.Typename))
            throw new YamlException("A component typename must be a non-empty scalar.");

        var catalog = MauiProgram.GetService<EngineService>().EngineTypes;
        if (!catalog.TryGetComponent(document.Typename, out var componentType))
            throw new YamlException($"Unknown component type '{document.Typename}'.");

        var component = new Component { Typename = componentType };
        foreach (var property in document.OverrideProperties ?? [])
        {
            var propertyAccess = EditorComponentPropertyContract.Classify(
                property.Key,
                componentType.Properties,
                componentType.ReadOnlyProperties);
            if (propertyAccess == EditorComponentPropertyAccess.ReadOnly)
            {
                component.PreservedReadOnlyProperties[property.Key] = property.Value;
                continue;
            }
            if (propertyAccess == EditorComponentPropertyAccess.Unknown)
            {
                throw new YamlException(
                    $"Unknown property '{property.Key}' for component type '{componentType.Name}'.");
            }

            var propType = componentType.Properties[property.Key];
            var scalar = Convert.ToString(property.Value, CultureInfo.InvariantCulture) ?? string.Empty;
            ObservableObject value = propType switch
            {
                RotationProperty => DeserializeBuffered<Rotation>(property.Value, serializer, deserializer),
                Vec4Property => DeserializeBuffered<Vec4>(property.Value, serializer, deserializer),
                Vec3Property => DeserializeBuffered<Vec3>(property.Value, serializer, deserializer),
                Vec2Property => DeserializeBuffered<Vec2>(property.Value, serializer, deserializer),
                FileIdProperty => new Observable<FileId>(scalar),
                InstanceIdProperty => new Observable<InstanceId>(scalar),
                FloatProperty => new Observable<float>((float)EditorComponentScalarCodec.Parse(
                    EditorComponentScalarKind.Float,
                    scalar)),
                ObjectPtrProperty => property.Value is null
                    ? new ObjectPtr()
                    : DeserializeBuffered<ObjectPtr>(property.Value, serializer, deserializer),
                EnumProperty => new Observable<string>((string)EditorComponentScalarCodec.Parse(
                    EditorComponentScalarKind.String,
                    scalar)),
                Property<string> => new Observable<string>((string)EditorComponentScalarCodec.Parse(
                    EditorComponentScalarKind.String,
                    scalar)),
                Property<bool> => new Observable<bool>((bool)EditorComponentScalarCodec.Parse(
                    EditorComponentScalarKind.Boolean,
                    scalar)),
                Property<int> => new Observable<int>((int)EditorComponentScalarCodec.Parse(
                    EditorComponentScalarKind.Int32,
                    scalar)),
                Property<uint> => new Observable<uint>((uint)EditorComponentScalarCodec.Parse(
                    EditorComponentScalarKind.UInt32,
                    scalar)),
                _ => throw new InvalidOperationException($"Unexpected property type: {propType.GetType().Name}")
            };

            component.OverrideProperties[property.Key] = value;
        }

        return component;
    }

    static T DeserializeBuffered<T>(object value, ISerializer serializer, IDeserializer deserializer)
        => deserializer.Deserialize<T>(serializer.Serialize(value));

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
        var serializer = SerializationUtils.CreateSerializerBuilder()            
            .Build();

        var component = (Component)value;

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        emitter.Emit(new Scalar(null, "typename"));
        emitter.Emit(new Scalar(null, component.Typename?.Name ??
            throw new InvalidOperationException("Cannot serialize a component without a reflected type.")));

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
                    emitter.Emit(new Scalar(null, EditorComponentScalarCodec.Format(
                        EditorComponentScalarKind.String,
                        str.Value)));
                    break;
                case Observable<bool> boolValue:
                    emitter.Emit(new Scalar(null, EditorComponentScalarCodec.Format(
                        EditorComponentScalarKind.Boolean,
                        boolValue.Value)));
                    break;
                case Observable<int> intValue:
                    emitter.Emit(new Scalar(null, EditorComponentScalarCodec.Format(
                        EditorComponentScalarKind.Int32,
                        intValue.Value)));
                    break;
                case Observable<uint> uintValue:
                    emitter.Emit(new Scalar(null, EditorComponentScalarCodec.Format(
                        EditorComponentScalarKind.UInt32,
                        uintValue.Value)));
                    break;
                case Observable<float> floatVal:
                    emitter.Emit(new Scalar(null, EditorComponentScalarCodec.Format(
                        EditorComponentScalarKind.Float,
                        floatVal.Value)));
                    break;
                case ObjectPtr objPtr:
                    objPtrConverter.WriteYaml(emitter, objPtr, typeof(ObjectPtr));
                    break;
                default:
                    throw new InvalidOperationException($"Unexpected property type: {kvp.Value.GetType().Name}");
            }
        }

        foreach (var kvp in component.PreservedReadOnlyProperties)
        {
            emitter.Emit(new Scalar(null, kvp.Key));
            serializer.Serialize(emitter, kvp.Value);
        }

        emitter.Emit(new MappingEnd());
        emitter.Emit(new MappingEnd());
    }
}
