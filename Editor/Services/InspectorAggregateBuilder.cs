using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Collections;
using System.Reflection;
using System.Runtime.CompilerServices;
using Component = SailorEditor.ViewModels.Component;

namespace SailorEditor.Services;

sealed class InspectorAggregateBuilder(WorldService world, AssetsService assets, EngineTypes engineTypes,
    ITransactionScopeFactory? transactions = null, IActionContextProvider? contextProvider = null)
{
    readonly long workspaceEpoch = world.WorkspaceEpoch;
    bool isSceneSelection;
    static readonly ConditionalWeakTable<ITransactionScopeFactory, SemaphoreSlim> SceneEditGates = new();
    static readonly Dictionary<string, string[]> AssetPropertyNames = new(StringComparer.Ordinal)
    {
        [nameof(TextureFile)] = ["Filtration", "Format", "Clamping", "ShouldGenerateMips", "ShouldSupportStorageBinding", "ShouldKeepCpuBuffers", "SamplerReduction"],
        [nameof(ModelFile)] = ["ShouldGenerateMaterials", "ShouldBatchByMaterial", "ShouldKeepCpuBuffers", "ShouldGenerateBLAS", "ShouldGenerateLods", "NumGeneratedLods", "LodReductionFactor", "UnitScale", "Materials", "Animations"],
        [nameof(MaterialFile)] = ["RenderQueue", "DepthBias", "SupportMultisampling", "CustomDepthShader", "EnableDepthTest", "EnableZWrite", "CullMode", "BlendMode", "FillMode", "Shader", "Samplers", "UniformsVec4", "UniformsFloat", "ShaderDefines"],
        [nameof(FrameGraphFile)] = ["Samplers", "Floats", "Vec4", "RenderTargets", "Nodes"]
    };

    public AggregateInspectorModel Build(IReadOnlyList<ObservableObject> selected)
    {
        isSceneSelection = selected.All(item => item is GameObject or Component);
        if (selected.All(item => item is GameObject))
        {
            return BuildGameObjects(selected.Cast<GameObject>().ToArray());
        }
        if (selected.All(item => item is Component))
        {
            var components = selected.Cast<Component>().ToArray();
            return new AggregateInspectorModel(selected.Count, [BuildComponents(components)]);
        }
        if (selected.All(item => item is AssetFile))
        {
            return BuildAssets(selected.Cast<AssetFile>().ToArray());
        }
        return new AggregateInspectorModel(selected.Count, []);
    }

    AggregateInspectorModel BuildGameObjects(IReadOnlyList<GameObject> selected)
    {
        var sources = selected.Select(CreateGameObjectSource).ToArray();
        var transformFields = new List<InspectorAggregateField>();
        foreach (var name in new[] { nameof(GameObject.Position), nameof(GameObject.Rotation), nameof(GameObject.Scale), nameof(GameObject.MobilityType) })
        {
            var property = typeof(GameObject).GetProperty(name)!;
            var choices = name == nameof(GameObject.MobilityType) ? GameObjectMobilityPolicy.Values.ToArray() : null;
            transformFields.Add(BuildField(name, sources.Select(source => SelectProperty(source, property)).ToArray(), choices: choices,
                xyzOnly: name is nameof(GameObject.Position) or nameof(GameObject.Scale)));
        }

        var nameProperty = typeof(GameObject).GetProperty(nameof(GameObject.Name))!;
        var sections = new List<InspectorAggregateSection>
        {
            new("Object", [BuildField("Name", sources.Select(source => SelectProperty(source, nameProperty)).ToArray())]),
            new("Transform", transformFields)
        };
        var componentSets = selected.Select(gameObject => world.GetComponents(gameObject)
            .GroupBy(component => component.Typename.Name)
            .ToDictionary(group => group.Key, group => group.ToArray(), StringComparer.Ordinal)).ToArray();
        foreach (var typeName in componentSets[0].Keys.Where(typeName => componentSets.All(set => set.ContainsKey(typeName))))
        {
            var count = componentSets.Min(set => set[typeName].Length);
            for (var index = 0; index < count; ++index)
            {
                sections.Add(BuildComponents(componentSets.Select(set => set[typeName][index]).ToArray(), count > 1 ? index + 1 : null));
            }
        }

        var ids = selected.Select(item => new InstanceId(item.InstanceId.Value)).ToArray();
        sections.Add(new InspectorAggregateSection("Components", [],
        [
            new("Add Component", async cancellationToken =>
            {
                var typeName = await Views.AddComponentDialogPage.ShowAsync(engineTypes.GetAddableComponentTypeNames());
                if (string.IsNullOrWhiteSpace(typeName))
                {
                    return;
                }
                await RunSceneActionAsync($"Add {typeName}", async () =>
                {
                    foreach (var id in ids)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        if (world.WorkspaceEpoch != workspaceEpoch || !world.TryGetGameObject(id, out var gameObject) ||
                            !await world.AddComponentAsync(gameObject, typeName, cancellationToken))
                        {
                            return false;
                        }
                    }
                    return true;
                }, cancellationToken);
            })
        ]));
        return new AggregateInspectorModel(selected.Count, sections);
    }

    InspectorAggregateSection BuildComponents(IReadOnlyList<Component> components, int? occurrence = null)
    {
        var commonNames = components[0].OverrideProperties.Keys.Where(name => InspectorPropertyPresentation.IsVisibleProperty(components[0].Typename.Name, name) &&
            components.All(component => !component.IsUndefined && component.OverrideProperties.ContainsKey(name) &&
                component.Typename.Properties.ContainsKey(name))).ToArray();
        var fields = new List<InspectorAggregateField>();
        foreach (var name in commonNames)
        {
            var descriptors = components.Select(component => component.Typename.Properties[name]).ToArray();
            if (descriptors.Any(descriptor => descriptor.GetType() != descriptors[0].GetType() || descriptor.Typename != descriptors[0].Typename))
            {
                continue;
            }

            var sources = components.Select(component => CreateComponentPropertySource(component, name)).ToArray();
            if (!CompatibleValues(sources))
            {
                continue;
            }
            fields.Add(BuildField(InspectorPropertyPresentation.FormatPropertyName(components[0].Typename.Name, name), sources, descriptors[0]));
        }

        var ids = components.Select(component => new InstanceId(component.InstanceId.Value)).ToArray();
        async Task ForEach(string actionName, Func<Component, CancellationToken, Task<bool>> action, CancellationToken cancellationToken)
        {
            await RunSceneActionAsync(actionName, async () =>
            {
                foreach (var id in ids)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (world.WorkspaceEpoch != workspaceEpoch || !world.TryGetComponent(id, out var component) ||
                        !await action(component, cancellationToken))
                    {
                        return false;
                    }
                }
                return true;
            }, cancellationToken);
        }

        var title = components.Select(component => component.Typename.Name).Distinct().Count() == 1
            ? components[0].Typename.Name.Split("::").Last()
            : "Common Properties";
        if (occurrence is not null)
        {
            title += $" ({occurrence})";
        }
        return new InspectorAggregateSection(title, fields,
        [
            new("Paste Values", token => ForEach("Paste Values", async (component, cancellationToken) =>
            {
                await MauiProgram.GetService<ComponentClipboardService>().PasteValuesAsync(component, cancellationToken);
                return true;
            }, token)),
            new("Reset to Defaults", token => ForEach("Reset to Defaults", world.ResetComponentToDefaultsAsync, token)),
            new("Remove Component", token => ForEach("Remove Component", world.RemoveComponentAsync, token))
        ]);
    }

    AggregateInspectorModel BuildAssets(IReadOnlyList<AssetFile> selected)
    {
        var properties = selected.Select(GetAssetProperties).ToArray();
        var fields = new List<InspectorAggregateField>();
        foreach (var name in properties[0].Keys.Where(name => properties.All(set => set.ContainsKey(name))))
        {
            var sources = properties.Select(set => set[name].Source).ToArray();
            if (!CompatibleValues(sources))
            {
                continue;
            }
            var descriptors = properties.Select(set => set[name].Descriptor).ToArray();
            var choices = properties.Select(set => set[name].Choices).ToArray();
            var commonChoices = choices.All(values => values is not null)
                ? choices[0]!.Where(value => choices.All(values => values!.Contains(value))).ToArray()
                : null;
            fields.Add(BuildField(name, sources, descriptors[0], commonChoices,
                selected.Any(asset => asset.IsReadOnly) || properties.Any(set => set[name].ReadOnly)));
        }

        return new AggregateInspectorModel(selected.Count, [new("Common Properties", fields)], true,
            () => selected.Any(asset => ResolveAsset(asset).IsDirty),
            async cancellationToken =>
            {
                // Keep each dirty original available while preceding saves publish new asset models.
                var saveTargets = selected.Select(ResolveAsset).Where(asset => asset.IsDirty && !asset.IsReadOnly).ToArray();
                return await assets.SaveAssetsAsync(saveTargets, cancellationToken);
            },
            async cancellationToken =>
            {
                foreach (var original in selected)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    var asset = ResolveAsset(original);
                    await asset.Revert();
                    await asset.PrepareInspectorResources();
                }
            });
    }

    sealed record AssetProperty(Source Source, PropertyBase? Descriptor, string[]? Choices = null, bool ReadOnly = false);

    Dictionary<string, AssetProperty> GetAssetProperties(AssetFile asset)
    {
        var result = new Dictionary<string, AssetProperty>(StringComparer.Ordinal);
        if (AssetPropertyNames.TryGetValue(asset.GetType().Name, out var names))
        {
            foreach (var name in names)
            {
                var property = asset.GetType().GetProperty(name);
                if (property is null)
                {
                    continue;
                }
                var enumPropertyName = name switch
                {
                    "Filtration" => "TextureFiltrationType",
                    "Clamping" => "TextureClampingType",
                    "Format" => "TextureFormatType",
                    "SamplerReduction" => "SamplerReductionType",
                    _ => name + "EnumValues"
                };
                var choices = (asset.GetType().GetProperty(enumPropertyName)?.GetValue(asset) as IEnumerable<string>)?.ToArray();
                result.Add(name, new AssetProperty(CreateAssetSource(asset, property), null, choices));
            }
            return result;
        }

        asset.EnsureAssetPropertiesTyped();
        foreach (var property in asset.AssetProperties)
        {
            if (property.Key is "assetInfoType" or "fileId" or "filename" || asset.TransientAssetProperties.Contains(property.Key))
            {
                continue;
            }
            var name = property.Key;
            PropertyBase? descriptor = null;
            asset.AssetType?.Properties.TryGetValue(name, out descriptor);
            result.Add(name, new AssetProperty(new Source(
                () => ResolveAsset(asset).AssetProperties.TryGetValue(name, out var value) ? value : null,
                (value, _) =>
                {
                    var target = ResolveAsset(asset);
                    target.AssetProperties[name] = (ObservableObject)value!;
                    target.MarkDirty(nameof(AssetFile.AssetProperties));
                    return Task.FromResult(true);
                }), descriptor, ReadOnly: descriptor is null || asset.ReadOnlyAssetProperties.Contains(name)));
        }
        return result;
    }

    Source CreateGameObjectSource(GameObject selected)
    {
        var id = new InstanceId(selected.InstanceId.Value);
        return new Source(
            () => world.WorkspaceEpoch == workspaceEpoch && world.TryGetGameObject(id, out var target) ? target : null,
            async (value, cancellationToken) =>
            {
                if (world.WorkspaceEpoch != workspaceEpoch || value is not GameObject target)
                {
                    return false;
                }
                var committed = await target.CommitInspectorChangesAsync(cancellationToken);
                return committed || !target.HasPendingInspectorChanges;
            }, CloneBeforeMutation: false);
    }

    Source CreateComponentPropertySource(Component selected, string propertyName)
    {
        var id = new InstanceId(selected.InstanceId.Value);
        return new Source(
            () => world.WorkspaceEpoch == workspaceEpoch && world.TryGetComponent(id, out var target) && target.OverrideProperties.TryGetValue(propertyName, out var value) ? value : null,
            async (value, cancellationToken) =>
            {
                if (world.WorkspaceEpoch != workspaceEpoch || !world.TryGetComponent(id, out var target))
                {
                    return false;
                }
                var committed = await target.ApplyInspectorBatchAsync(() =>
                {
                    target.OverrideProperties[propertyName] = (ObservableObject)value!;
                }, cancellationToken);
                return committed || !target.HasPendingInspectorChanges;
            });
    }

    AssetFile ResolveAsset(AssetFile original) => !original.IsDirty &&
        assets.Assets.TryGetValue(original.FileId, out var current) && current.GetType() == original.GetType()
            ? current
            : original;

    Source CreateAssetSource(AssetFile asset, PropertyInfo property) => new(
        () => property.GetValue(ResolveAsset(asset)),
        (value, _) =>
        {
            var target = ResolveAsset(asset);
            property.SetValue(target, value);
            target.MarkDirty(property.Name);
            return Task.FromResult(true);
        }, DeclaredType: property.PropertyType);

    InspectorAggregateField BuildField(
        string name,
        IReadOnlyList<Source> sources,
        PropertyBase? descriptor = null,
        string[]? choices = null,
        bool readOnly = false,
        bool xyzOnly = false)
    {
        var sample = sources[0].Read();
        if (sample is not null && sample.GetType().IsGenericType && sample.GetType().GetGenericTypeDefinition() == typeof(Observable<>))
        {
            var valueProperty = sample.GetType().GetProperty("Value")!;
            return BuildField(name, sources.Select(source => SelectProperty(source, valueProperty)).ToArray(), descriptor, choices, readOnly);
        }

        var children = new List<InspectorAggregateField>();
        var actions = new List<InspectorAggregateAction>();
        if (sample is ObservableRecord record)
        {
            foreach (var fieldName in record.Fields.Keys.Where(key => sources.All(source => source.Read() is ObservableRecord value && value.Fields.ContainsKey(key))))
            {
                var childSources = sources.Select(source => source.Select(
                    value => ((ObservableRecord)value!).Fields[fieldName],
                    (value, child) => ((ObservableRecord)value!).Fields[fieldName] = (ObservableObject)child!)).ToArray();
                var childDescriptor = (descriptor as RecordProperty)?.RecordType.Properties.GetValueOrDefault(fieldName);
                if (CompatibleValues(childSources))
                {
                    children.Add(BuildField(fieldName, childSources, childDescriptor, readOnly: readOnly));
                }
            }
        }
        else if (TryGetList(sample, out var list))
        {
            var count = sources.Min(source => TryGetList(source.Read(), out var values) ? values.Count : 0);
            var elementDescriptor = (descriptor as ListProperty)?.ElementType;
            for (var index = 0; index < count; ++index)
            {
                var elementIndex = index;
                var childSources = sources.Select(source => source.Select(
                    value => GetList(value)[elementIndex],
                    (value, child) => GetList(value)[elementIndex] = child)).ToArray();
                if (CompatibleValues(childSources))
                {
                    children.Add(BuildField($"[{index + 1}]", childSources, elementDescriptor, readOnly: readOnly));
                }
                if (!readOnly)
                {
                    actions.Add(new($"Remove [{index + 1}]", token => MutateListsAsGroupAsync(name, sources, values => values.RemoveAt(elementIndex), token)));
                    if (index > 0)
                    {
                        actions.Add(new($"Up [{index + 1}]", token => MutateListsAsGroupAsync(name, sources, values => MoveListItem(values, elementIndex, elementIndex - 1), token)));
                    }
                    if (index + 1 < count)
                    {
                        actions.Add(new($"Down [{index + 1}]", token => MutateListsAsGroupAsync(name, sources, values => MoveListItem(values, elementIndex, elementIndex + 1), token)));
                    }
                }
            }
            if (!readOnly)
            {
                actions.Add(new("Add", token => MutateListsAsGroupAsync(name, sources, values => values.Add(CreateListElement(values, elementDescriptor)), token)));
                actions.Add(new("Clear", token => MutateListsAsGroupAsync(name, sources, values => values.Clear(), token)));
            }
        }
        else if (sample is Vec2 or Vec3 or Vec4 or Rotation or Quat or ObjectPtr)
        {
            var propertyNames = sample switch
            {
                Rotation => new[] { "Yaw", "Pitch", "Roll" },
                ObjectPtr => new[] { "FileId", "InstanceId" },
                Vec2 => new[] { "X", "Y" },
                Vec3 => new[] { "X", "Y", "Z" },
                Vec4 when xyzOnly => new[] { "X", "Y", "Z" },
                _ => new[] { "X", "Y", "Z", "W" }
            };
            foreach (var propertyName in propertyNames)
            {
                var property = sample.GetType().GetProperty(propertyName)!;
                children.Add(BuildField(propertyName, sources.Select(source => SelectProperty(source, property)).ToArray(),
                    descriptor: sample is ObjectPtr ? descriptor : null, readOnly: readOnly));
            }
        }
        else if (sample is ObservableObject && !IsScalar(sample.GetType()))
        {
            foreach (var property in sample.GetType().GetProperties().Where(property => property.GetIndexParameters().Length == 0 &&
                         property.CanRead && property.CanWrite && IsEditablePropertyType(property.PropertyType)))
            {
                children.Add(BuildField(property.Name, sources.Select(source => SelectProperty(source, property)).ToArray(), readOnly: readOnly));
            }
        }

        if (choices is null && descriptor is EnumProperty enumeration)
        {
            choices = engineTypes.Enums.GetValueOrDefault(enumeration.Typename)?.ToArray();
        }
        return new InspectorAggregateField(name, sample?.GetType() ?? sources[0].DeclaredType ?? typeof(string),
            sources.Select(source => new InspectorAggregateValueTarget(source.Read, source.Write)).ToArray())
        {
            Choices = choices ?? [],
            Range = descriptor?.Range,
            IsReadOnly = readOnly || (children.Count == 0 && actions.Count == 0 && sample is not null && !IsScalar(sample.GetType())),
            Children = children,
            Actions = actions,
            SupportedAssetType = descriptor is ObjectPtrProperty pointer ? pointer.GenericType : null,
            ExpectedInstanceType = descriptor is ObjectPtrProperty objectPointer ? objectPointer.GenericTypename : string.Empty,
            ApplyGroup = isSceneSelection && transactions is not null && contextProvider is not null
                ? (apply, token) => ApplySceneEditGroupAsync(name, apply, token)
                : null
        };
    }

    async Task<bool> ApplySceneEditGroupAsync(string name, Func<Task<bool>> apply, CancellationToken token)
    {
        var gate = SceneEditGates.GetValue(transactions!, _ => new SemaphoreSlim(1, 1));
        await gate.WaitAsync(token);
        try
        {
            var context = contextProvider!.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(InspectorAggregateBuilder)));
            await using var transaction = await transactions!.BeginAsync($"Edit {name} on selection", context, token);
            if (!await apply())
            {
                return false;
            }
            await transaction.CompleteAsync(token);
            return true;
        }
        finally
        {
            gate.Release();
        }
    }

    async Task RunSceneActionAsync(string name, Func<Task<bool>> apply, CancellationToken token)
    {
        var succeeded = transactions is not null && contextProvider is not null
            ? await ApplySceneEditGroupAsync(name, apply, token)
            : await apply();
        if (!succeeded)
        {
            throw new InvalidOperationException($"{name} could not be applied to every selected target.");
        }
    }

    Task MutateListsAsGroupAsync(string name, IReadOnlyList<Source> sources, Action<IList> mutation, CancellationToken token)
    {
        if (isSceneSelection)
        {
            return RunSceneActionAsync($"Edit {name}", async () =>
            {
                await MutateListsAsync(sources, mutation, token);
                return true;
            }, token);
        }
        return MutateListsAsync(sources, mutation, token);
    }

    static void MoveListItem(IList values, int from, int to)
    {
        var value = values[from];
        values.RemoveAt(from);
        values.Insert(to, value);
    }

    static bool IsScalar(Type type) => type == typeof(string) || type == typeof(float) || type == typeof(int) ||
        type == typeof(uint) || type == typeof(bool) || type == typeof(FileId) || type == typeof(InstanceId);

    static bool IsEditablePropertyType(Type type) => IsScalar(type) || typeof(ObservableObject).IsAssignableFrom(type) || typeof(IList).IsAssignableFrom(type);

    object? CreateListElement(IList list, PropertyBase? descriptor)
    {
        if (descriptor is not null)
        {
            return new ReflectedValueCodec(engineTypes).CreateDefault(descriptor);
        }
        var elementType = list.GetType().GetGenericArguments().FirstOrDefault();
        if (elementType is null)
        {
            throw new InvalidOperationException("The list element type is unavailable.");
        }
        if (elementType.IsGenericType && elementType.GetGenericTypeDefinition() == typeof(Observable<>))
        {
            var scalarType = elementType.GetGenericArguments()[0];
            var defaultValue = scalarType == typeof(string) ? string.Empty : Activator.CreateInstance(scalarType);
            return Activator.CreateInstance(elementType, [defaultValue]);
        }
        return elementType == typeof(string) ? string.Empty : Activator.CreateInstance(elementType);
    }

    static async Task MutateListsAsync(IReadOnlyList<Source> sources, Action<IList> mutation, CancellationToken token)
    {
        foreach (var source in sources)
        {
            token.ThrowIfCancellationRequested();
            var value = InspectorAggregateField.CloneValue(source.Read());
            mutation(GetList(value));
            if (!await source.Write(value, token))
            {
                throw new InvalidOperationException("The list edit could not be committed to every selected target.");
            }
        }
    }

    static IList GetList(object? value) => TryGetList(value, out var values) ? values : throw new InvalidOperationException("The selected list is no longer available.");

    static bool TryGetList(object? value, out IList list)
    {
        list = value switch
        {
            ObservableValueList values => values.Values,
            ObservableFileIdList values => values.Values,
            ObservableFloatList values => values.Values,
            IList values => values,
            _ => null!
        };
        return list is not null;
    }

    static Source SelectProperty(Source source, PropertyInfo property) => source.Select(
        value => value is null ? null : property.GetValue(value),
        (value, child) => property.SetValue(value, child), property.PropertyType);

    static bool CompatibleValues(IReadOnlyList<Source> sources)
    {
        var type = sources[0].Read()?.GetType() ?? sources[0].DeclaredType;
        return sources.All(source => (source.Read()?.GetType() ?? source.DeclaredType) == type);
    }

    sealed record Source(Func<object?> Read, Func<object?, CancellationToken, Task<bool>> Write,
        bool CloneBeforeMutation = true, Type? DeclaredType = null)
    {
        public Source Select(Func<object?, object?> read, Action<object?, object?> write, Type? declaredType = null) => new(
            () => Read() is { } parent ? read(parent) : null,
            async (child, token) =>
            {
                var parent = Read();
                if (parent is null)
                {
                    return false;
                }
                if (CloneBeforeMutation)
                {
                    parent = InspectorAggregateField.CloneValue(parent);
                }
                write(parent, child);
                return await Write(parent, token);
            }, DeclaredType: declaredType);
    }
}
