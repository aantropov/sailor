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
        propertyName => propertyName == nameof(OverrideProperties));
    readonly SemaphoreSlim _commitGate = new(1, 1);
    int pendingInspectorCommits;
    int inspectorBatchDepth;

    public Component()
    {
        PropertyChanged += (s, args) =>
        {
            var decision = _autoCommit.OnPropertyChanged(args.PropertyName);
            if (!decision.MarkDirty)
                return;

            IsDirty = true;
            if (decision.CommitNow && Volatile.Read(ref inspectorBatchDepth) == 0)
                _ = CommitInspectorChangesSafelyAsync();
        };
    }

    public async Task<bool> ApplyInspectorBatchAsync(
        Action mutation,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(mutation);
        Interlocked.Increment(ref inspectorBatchDepth);
        try
        {
            mutation();
        }
        finally
        {
            Interlocked.Decrement(ref inspectorBatchDepth);
        }

        return await CommitInspectorChangesAsync(cancellationToken);
    }

    public async Task<bool> CommitInspectorChangesAsync(
        CancellationToken cancellationToken = default)
    {
        Interlocked.Increment(ref pendingInspectorCommits);
        var acquired = false;
        try
        {
            await _commitGate.WaitAsync(cancellationToken);
            acquired = true;
            return await CommitInspectorChangesCoreAsync(
                cancellationToken);
        }
        finally
        {
            if (acquired)
            {
                _commitGate.Release();
            }
            Interlocked.Decrement(ref pendingInspectorCommits);
        }
    }

    async Task<bool> CommitInspectorChangesCoreAsync(
        CancellationToken cancellationToken)
    {
        if (!isInited)
            return false;
        if (!_autoCommit.ShouldCommitPendingChanges(IsDirty))
            return true;

        var yamlComponent = EditorYaml.SerializeComponent(this);
        var previousYaml = _lastCommittedYaml ?? yamlComponent;
        if (string.Equals(previousYaml, yamlComponent, StringComparison.Ordinal))
        {
            IsDirty = false;
            return false;
        }

        var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
        var contextProvider = MauiProgram.GetService<IActionContextProvider>();
        IsDirty = false;

        CommandResult result;
        try
        {
            result = await dispatcher.DispatchAsync(
                new UpdateComponentCommand(this, previousYaml, yamlComponent, $"Edit {Typename?.Name}"),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(CommitInspectorChangesAsync))),
                cancellationToken);
        }
        catch
        {
            IsDirty = true;
            throw;
        }

        if (result.Succeeded)
        {
            _lastCommittedYaml = yamlComponent;
            return true;
        }

        IsDirty = true;
        return false;
    }

    async Task CommitInspectorChangesSafelyAsync()
    {
        try
        {
            await CommitInspectorChangesAsync();
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Automatic component inspector commit failed: {exception}");
        }
    }

    [YamlIgnore]
    public bool HasPendingInspectorChanges =>
        IsDirty || Volatile.Read(ref pendingInspectorCommits) != 0;

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
    readonly IDeserializer bufferedDeserializer = SerializationUtils
        .CreateDeserializerBuilder()
        .Build();
    readonly ISerializer bufferedSerializer = SerializationUtils
        .CreateSerializerBuilder()
        .Build();

    public bool Accepts(Type type) => type == typeof(Component);

    public object ReadYaml(IParser parser, Type type)
    {
        var document = bufferedDeserializer.Deserialize<EditorComponentYamlContract>(parser) ??
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
                RotationProperty => DeserializeBuffered<Rotation>(property.Value, bufferedSerializer, bufferedDeserializer),
                Vec4Property => DeserializeBuffered<Vec4>(property.Value, bufferedSerializer, bufferedDeserializer),
                Vec3Property => DeserializeBuffered<Vec3>(property.Value, bufferedSerializer, bufferedDeserializer),
                Vec2Property => DeserializeBuffered<Vec2>(property.Value, bufferedSerializer, bufferedDeserializer),
                FileIdProperty => new Observable<FileId>(scalar),
                Property<List<FileId>> => DeserializeFileIdList(
                    property.Value,
                    bufferedSerializer,
                    bufferedDeserializer),
                Property<List<float>> => DeserializeFloatList(
                    property.Value,
                    bufferedSerializer,
                    bufferedDeserializer),
                InstanceIdProperty => new Observable<InstanceId>(scalar),
                FloatProperty => new Observable<float>((float)EditorComponentScalarCodec.Parse(
                    EditorComponentScalarKind.Float,
                    scalar)),
                ObjectPtrProperty => property.Value is null
                    ? new ObjectPtr()
                    : DeserializeBuffered<ObjectPtr>(property.Value, bufferedSerializer, bufferedDeserializer),
                EnumProperty enumProperty => new Observable<string>(ParseEnumOverride(
                    catalog,
                    componentType,
                    property.Key,
                    enumProperty,
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

        AddMissingLandscapeVegetationProperties(component, document.Typename);

        return component;
    }

    static void AddMissingLandscapeVegetationProperties(
        Component component,
        string componentTypeName)
    {
        if (!string.Equals(
                componentTypeName,
                "Sailor::LandscapeComponent",
                StringComparison.Ordinal))
        {
            return;
        }

        if (component.Typename.Properties.ContainsKey("heightmapTexture") &&
            !component.OverrideProperties.ContainsKey("heightmapTexture"))
        {
            component.OverrideProperties["heightmapTexture"] =
                new Observable<FileId>(new FileId());
        }

        if (component.Typename.Properties.ContainsKey("materialMasks") &&
            !component.OverrideProperties.ContainsKey("materialMasks"))
        {
            component.OverrideProperties["materialMasks"] = new ObservableFileIdList();
        }

        if (component.Typename.Properties.ContainsKey("lodDistances") &&
            !component.OverrideProperties.ContainsKey("lodDistances"))
        {
            component.OverrideProperties["lodDistances"] =
                new ObservableFloatList([96.0f, 192.0f]);
        }
        if (component.Typename.Properties.ContainsKey("lodSkirtDepth") &&
            !component.OverrideProperties.ContainsKey("lodSkirtDepth"))
        {
            component.OverrideProperties["lodSkirtDepth"] = new Observable<float>(2.0f);
        }
        if (component.Typename.Properties.ContainsKey("grassResidencyHysteresis") &&
            !component.OverrideProperties.ContainsKey("grassResidencyHysteresis"))
        {
            component.OverrideProperties["grassResidencyHysteresis"] = new Observable<float>(12.0f);
        }

        if (!component.OverrideProperties.TryGetValue(
                "vegetationModels",
                out var modelsProperty) ||
            modelsProperty is not ObservableFileIdList models)
        {
            return;
        }

        (string Name, float DefaultValue)[] properties =
        [
            ("vegetationMeshIndex", -1.0f),
            ("vegetationResidency", 0.0f),
            ("vegetationPriority", 1.0f),
            ("vegetationMinLod", 0.0f),
            ("vegetationMaxLod", 2.0f),
            ("vegetationLod1ScreenCoverage", 0.25f),
            ("vegetationLod2ScreenCoverage", 0.05f),
            ("vegetationCullDistance", 120.0f),
            ("vegetationColliderRadius", 0.0f),
            ("vegetationColliderHeight", 2.0f),
            ("vegetationColliderOffsetY", 1.0f)
        ];

        foreach (var property in properties)
        {
            if (!component.Typename.Properties.ContainsKey(property.Name) ||
                component.OverrideProperties.ContainsKey(property.Name))
            {
                continue;
            }

            component.OverrideProperties[property.Name] = new ObservableFloatList(
                Enumerable.Repeat(property.DefaultValue, models.Values.Count));
        }
    }

    static string ParseEnumOverride(
        EngineTypes catalog,
        ComponentType componentType,
        string propertyName,
        EnumProperty enumProperty,
        string scalar)
    {
        if (!catalog.Enums.TryGetValue(enumProperty.Typename, out var allowedValues))
        {
            throw new YamlException(
                $"Missing enum metadata '{enumProperty.Typename}' for " +
                $"'{componentType.Name}.{propertyName}'.");
        }

        var value = (string)EditorComponentScalarCodec.Parse(
            EditorComponentScalarKind.String,
            scalar);
        try
        {
            return EditorComponentPropertyContract.ValidateEnumValue(
                componentType.Name,
                propertyName,
                enumProperty.Typename,
                value,
                allowedValues);
        }
        catch (InvalidDataException ex)
        {
            throw new YamlException(ex.Message);
        }
    }

    static T DeserializeBuffered<T>(object value, ISerializer serializer, IDeserializer deserializer)
        => deserializer.Deserialize<T>(serializer.Serialize(value));

    static ObservableFileIdList DeserializeFileIdList(
        object? value,
        ISerializer serializer,
        IDeserializer deserializer)
    {
        if (value is null)
            return new ObservableFileIdList();

        return new ObservableFileIdList(
            DeserializeBuffered<List<FileId>>(
                value,
                serializer,
                deserializer) ?? []);
    }

    static ObservableFloatList DeserializeFloatList(
        object? value,
        ISerializer serializer,
        IDeserializer deserializer)
    {
        if (value is null)
            return new ObservableFloatList();

        return new ObservableFloatList(
            DeserializeBuffered<List<float>>(
                value,
                serializer,
                deserializer) ?? []);
    }

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
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
                case ObservableFileIdList assetIds:
                    emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
                    foreach (var assetId in assetIds.Values)
                    {
                        fileIdConverter.WriteYaml(
                            emitter,
                            assetId.Value ?? new FileId(),
                            typeof(FileId));
                    }
                    emitter.Emit(new SequenceEnd());
                    break;
                case ObservableFloatList floatValues:
                    emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
                    foreach (var floatValue in floatValues.Values)
                    {
                        emitter.Emit(new Scalar(null, EditorComponentScalarCodec.Format(
                            EditorComponentScalarKind.Float,
                            floatValue.Value)));
                    }
                    emitter.Emit(new SequenceEnd());
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
            bufferedSerializer.Serialize(emitter, kvp.Value);
        }

        emitter.Emit(new MappingEnd());
        emitter.Emit(new MappingEnd());
    }
}
