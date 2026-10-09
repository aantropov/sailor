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
        propertyName => propertyName is nameof(IsDirty) or nameof(DisplayName),
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

    [YamlIgnore]
    public bool HasInFlightInspectorCommit =>
        Volatile.Read(ref pendingInspectorCommits) != 0;

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

    public InstanceId InstanceId => IsUndefined
        ? new InstanceId((string)PreservedReadOnlyProperties["instanceId"]!)
        : OverrideProperties["instanceId"] as Observable<InstanceId>;

    [YamlIgnore]
    public bool IsUndefined { get; init; }

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
    readonly IValueSerializer bufferedValueSerializer = SerializationUtils
        .CreateSerializerBuilder()
        .BuildValueSerializer();

    public bool Accepts(Type type) => type == typeof(Component);

    public object ReadYaml(IParser parser, Type type)
    {
        var document = bufferedDeserializer.Deserialize<EditorComponentYamlContract>(parser) ??
            throw new YamlException("A component YAML document is required.");
        if (string.IsNullOrWhiteSpace(document.Typename))
            throw new YamlException("A component typename must be a non-empty scalar.");

        var catalog = MauiProgram.GetService<EngineService>().EngineTypes;
        if (!catalog.TryGetComponent(document.Typename, out var componentType))
        {
            return PreserveUndefined(document);
        }

        var component = new Component { Typename = componentType };
        var values = new ReflectedValueCodec(catalog);
        try
        {
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

                component.OverrideProperties[property.Key] = values.Read(
                    componentType.Properties[property.Key], property.Value, componentType.Name, property.Key);
            }
        }
        catch (Exception error) when (error is YamlException or InvalidDataException)
        {
            Console.Error.WriteLine($"Cannot read component '{document.Typename}': {error.Message}");
            return PreserveUndefined(document);
        }

        return component;
    }

    static Component PreserveUndefined(EditorComponentYamlContract document)
    {
        var component = new Component
        {
            Typename = new ComponentType { Name = document.Typename },
            IsUndefined = true
        };
        foreach (var property in document.OverrideProperties ?? [])
        {
            component.PreservedReadOnlyProperties[property.Key] = property.Value;
        }
        return component;
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

        var values = new ReflectedValueCodec(MauiProgram.GetService<EngineService>().EngineTypes);
        foreach (var kvp in component.OverrideProperties)
        {
            emitter.Emit(new Scalar(null, kvp.Key));
            values.Write(emitter, kvp.Value);
        }

        foreach (var kvp in component.PreservedReadOnlyProperties)
        {
            emitter.Emit(new Scalar(null, kvp.Key));
            bufferedValueSerializer.SerializeValue(emitter, kvp.Value, null);
        }

        emitter.Emit(new MappingEnd());
        emitter.Emit(new MappingEnd());
    }
}
