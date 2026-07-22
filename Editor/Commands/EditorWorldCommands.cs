using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.History;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using YamlDotNet.Serialization;

namespace SailorEditor.Commands;

public static class EditorYaml
{
    public static string SerializeGameObject(GameObject gameObject) => SerializationUtils.CreateSerializerBuilder().Build().Serialize(gameObject);

    public static string SerializeComponent(Component component) => SerializationUtils.CreateSerializerBuilder().WithTypeConverter(new ComponentYamlConverter()).Build().Serialize(component);

    public static string SerializePrefab(Prefab prefab)
    {
        IYamlTypeConverter[] commonConverters =
        [
            new ComponentTypeYamlConverter(),
            new ComponentYamlConverter()
        ];

        var serializerBuilder = SerializationUtils.CreateSerializerBuilder()
            .WithTypeConverter(new ObservableListConverter<GameObject>(commonConverters))
            .WithTypeConverter(new ObservableListConverter<Component>(commonConverters));

        foreach (var converter in commonConverters)
            serializerBuilder.WithTypeConverter(converter);

        return serializerBuilder.Build().Serialize(prefab);
    }
}

public sealed class SelectObjectCommand(InstanceId? instanceId = null, ObservableObject? selectedObject = null) : IEditorCommand
{
    readonly InstanceId? _instanceId = instanceId;
    readonly ObservableObject? _selectedObject = selectedObject;

    public string Name => nameof(SelectObjectCommand);
    public bool CanExecute(ActionContext context) => _instanceId is null || !_instanceId.IsEmpty() || _selectedObject is not null;

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var selectionService = MauiProgram.GetService<SelectionService>();
        if (_selectedObject is not null)
        {
            selectionService.SelectObject(_selectedObject);
            return Task.FromResult(CommandResult.Success());
        }

        if (_instanceId is null || _instanceId.IsEmpty())
        {
            selectionService.ClearSelection();
            return Task.FromResult(CommandResult.Success());
        }

        selectionService.SelectInstance(_instanceId);
        return Task.FromResult(CommandResult.Success());
    }
}

public sealed class UpdateGameObjectCommand : IHistoryCoalescibleCommand
{
    readonly InstanceId _instanceId;
    readonly string _beforeYaml;
    readonly string _afterYaml;

    public UpdateGameObjectCommand(GameObject gameObject, string beforeYaml, string afterYaml, string description = "Edit object")
        : this(new InstanceId(gameObject.InstanceId?.Value ?? InstanceId.NullInstanceId), beforeYaml, afterYaml, description)
    {
    }

    UpdateGameObjectCommand(InstanceId instanceId, string beforeYaml, string afterYaml, string description)
    {
        _instanceId = instanceId;
        _beforeYaml = beforeYaml;
        _afterYaml = afterYaml;
        Description = description;
    }

    public string Name => nameof(UpdateGameObjectCommand);
    public string Description { get; }
    public IHistoryMergePolicy? MergePolicy => new TimeWindowHistoryMergePolicy(TimeSpan.FromMilliseconds(750));
    public bool CanExecute(ActionContext context) => _instanceId is not null && !_instanceId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(Apply(_afterYaml));
    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        => ValueTask.FromResult(Apply(_beforeYaml));
    public bool CanCoalesceWith(IUndoableEditorCommand next) =>
        next is UpdateGameObjectCommand command && _instanceId.Equals(command._instanceId);
    public IUndoableEditorCommand CoalesceWith(IUndoableEditorCommand next)
    {
        var command = (UpdateGameObjectCommand)next;
        return new UpdateGameObjectCommand(_instanceId, _beforeYaml, command._afterYaml, command.Description);
    }
    CommandResult Apply(string yaml)
    {
        if (!MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, yaml))
            return CommandResult.Failure();

        return MauiProgram.GetService<WorldService>().ApplyGameObjectYamlLocal(_instanceId, yaml)
            ? CommandResult.Success()
            : CommandResult.Failure();
    }
}

public sealed class UpdateComponentCommand : IHistoryCoalescibleCommand
{
    readonly InstanceId _instanceId;
    readonly string _beforeYaml;
    readonly string _afterYaml;

    public UpdateComponentCommand(Component component, string beforeYaml, string afterYaml, string description = "Edit component")
        : this(new InstanceId(component.InstanceId?.Value ?? InstanceId.NullInstanceId), beforeYaml, afterYaml, description)
    {
    }

    UpdateComponentCommand(InstanceId instanceId, string beforeYaml, string afterYaml, string description)
    {
        _instanceId = instanceId;
        _beforeYaml = beforeYaml;
        _afterYaml = afterYaml;
        Description = description;
    }

    public string Name => nameof(UpdateComponentCommand);
    public string Description { get; }
    public IHistoryMergePolicy? MergePolicy => new TimeWindowHistoryMergePolicy(TimeSpan.FromMilliseconds(750));
    public bool CanExecute(ActionContext context) => _instanceId is not null && !_instanceId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(Apply(_afterYaml));
    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
        => ValueTask.FromResult(Apply(_beforeYaml));
    public bool CanCoalesceWith(IUndoableEditorCommand next) =>
        next is UpdateComponentCommand command && _instanceId.Equals(command._instanceId);
    public IUndoableEditorCommand CoalesceWith(IUndoableEditorCommand next)
    {
        var command = (UpdateComponentCommand)next;
        return new UpdateComponentCommand(_instanceId, _beforeYaml, command._afterYaml, command.Description);
    }
    CommandResult Apply(string yaml)
    {
        if (!MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, yaml))
            return CommandResult.Failure();

        return MauiProgram.GetService<WorldService>().ApplyComponentYamlLocal(_instanceId, yaml)
            ? CommandResult.Success()
            : CommandResult.Failure();
    }
}

public sealed class CreateGameObjectCommand(InstanceId? parentId = null) : IUndoableEditorCommand
{
    InstanceId? _createdId;
    public string Name => nameof(CreateGameObjectCommand);
    public string Description => "Create GameObject";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => true;
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var world = MauiProgram.GetService<WorldService>();
        var engine = MauiProgram.GetService<EngineService>();
        if (!engine.CreateGameObject(parentId, _createdId, out var createdId))
            return Task.FromResult(CommandResult.Failure());

        _createdId = createdId;
        if (!world.TryGetGameObject(createdId, out _))
        {
            engine.DestroyObject(createdId);
            return Task.FromResult(CommandResult.Failure("Created object was not projected"));
        }

        return Task.FromResult(CommandResult.Success(value: _createdId));
    }
    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (_createdId is null || _createdId.IsEmpty())
            return ValueTask.FromResult(CommandResult.Failure("Created object was not found"));

        return ValueTask.FromResult(MauiProgram.GetService<EngineService>().DestroyObject(_createdId)
            ? CommandResult.Success()
            : CommandResult.Failure("Destroy created object failed"));
    }
}

public sealed class DestroyGameObjectCommand(GameObject gameObject) : IUndoableEditorCommand
{
    readonly string _name = gameObject.Name;
    readonly InstanceId? _parentId = gameObject.ParentIndex == uint.MaxValue
        ? null
        : MauiProgram.GetService<WorldService>().Current.Prefabs[gameObject.PrefabIndex].GameObjects[(int)gameObject.ParentIndex].InstanceId;
    readonly string _prefabSnapshotYaml = CreateSnapshotYaml(gameObject);
    InstanceId _activeInstanceId = gameObject.InstanceId;

    public string Name => nameof(DestroyGameObjectCommand);
    public string Description => $"Delete {gameObject.Name}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _activeInstanceId is not null && !_activeInstanceId.IsEmpty() && !string.IsNullOrWhiteSpace(_prefabSnapshotYaml);

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        => Task.FromResult(MauiProgram.GetService<EngineService>().DestroyObject(_activeInstanceId) ? CommandResult.Success() : CommandResult.Failure("Destroy failed"));

    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        var beforeIds = world.Current.Prefabs.SelectMany(x => x.GameObjects).Select(x => x.InstanceId?.Value).ToHashSet();

        if (!engine.InstantiatePrefabFromYaml(_prefabSnapshotYaml, _parentId))
            return ValueTask.FromResult(CommandResult.Failure("Restore deleted hierarchy failed"));

        if (world.TryGetGameObject(_activeInstanceId, out _))
            return ValueTask.FromResult(CommandResult.Success());

        var restored = world.Current.Prefabs
            .SelectMany(x => x.GameObjects)
            .Where(x => x.InstanceId is not null && !beforeIds.Contains(x.InstanceId.Value))
            .FirstOrDefault(x => x.Name == _name && ((_parentId is null && x.ParentIndex == uint.MaxValue) || (_parentId is not null && x.ParentIndex != uint.MaxValue && world.Current.Prefabs[x.PrefabIndex].GameObjects[(int)x.ParentIndex].InstanceId.Equals(_parentId))))
            ?? world.Current.Prefabs.SelectMany(x => x.GameObjects).FirstOrDefault(x => x.InstanceId is not null && !beforeIds.Contains(x.InstanceId.Value));

        if (restored is null)
            return ValueTask.FromResult(CommandResult.Failure("Restored hierarchy root was not found"));

        engine.DestroyObject(restored.InstanceId);
        return ValueTask.FromResult(CommandResult.Failure("Restored hierarchy did not preserve its instance identity"));
    }

    static string CreateSnapshotYaml(GameObject root)
    {
        var prefab = MauiProgram.GetService<WorldService>().CreatePrefabFromSubHierarchy(root, out _);
        return EditorYaml.SerializePrefab(prefab);
    }
}

public sealed class ReparentGameObjectCommand(GameObject child, GameObject? newParent, bool keepWorldTransform = true) : IUndoableEditorCommand
{
    readonly InstanceId _childId = child.InstanceId;
    readonly InstanceId? _newParentId = newParent?.InstanceId;
    readonly InstanceId? _oldParentId = child.ParentIndex == uint.MaxValue ? null : MauiProgram.GetService<WorldService>().Current.Prefabs[child.PrefabIndex].GameObjects[(int)child.ParentIndex].InstanceId;
    public string Name => nameof(ReparentGameObjectCommand);
    public string Description => "Reparent GameObject";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _childId is not null && !_childId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        if (!engine.ReparentObject(_childId, _newParentId, keepWorldTransform))
            return Task.FromResult(CommandResult.Failure());

        return Task.FromResult(CommandResult.Success());
    }

    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        return ValueTask.FromResult(engine.ReparentObject(_childId, _oldParentId, keepWorldTransform)
            ? CommandResult.Success()
            : CommandResult.Failure("Restore parent failed"));
    }
}

public sealed class AddComponentCommand(GameObject gameObject, string componentTypeName) : IUndoableEditorCommand
{
    readonly InstanceId _ownerId = gameObject.InstanceId;
    InstanceId? _componentId;
    public string Name => nameof(AddComponentCommand);
    public string Description => $"Add {componentTypeName}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _ownerId is not null && !_ownerId.IsEmpty() && !string.IsNullOrWhiteSpace(componentTypeName);
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var world = MauiProgram.GetService<WorldService>();
        if (!world.TryGetGameObject(_ownerId, out var owner))
            return Task.FromResult(CommandResult.Failure("Owner not found"));

        var engine = MauiProgram.GetService<EngineService>();
        if (!engine.AddComponent(_ownerId, componentTypeName, _componentId, out var createdId))
            return Task.FromResult(CommandResult.Failure());

        _componentId = createdId;
        if (!world.TryGetComponent(createdId, out _))
        {
            engine.RemoveComponent(createdId);
            return Task.FromResult(CommandResult.Failure("Created component was not projected"));
        }

        return Task.FromResult(CommandResult.Success(value: _componentId));
    }
    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (_componentId is null || _componentId.IsEmpty())
            return ValueTask.FromResult(CommandResult.Failure("Created component was not found"));

        return ValueTask.FromResult(MauiProgram.GetService<EngineService>().RemoveComponent(_componentId)
            ? CommandResult.Success()
            : CommandResult.Failure("Remove created component failed"));
    }
}

public sealed class RemoveComponentCommand(Component component) : IUndoableEditorCommand
{
    readonly InstanceId _ownerId = MauiProgram.GetService<WorldService>().FindOwner(component)?.InstanceId;
    readonly string _beforeYaml = EditorYaml.SerializeComponent(component);
    readonly string _componentTypeName = component.Typename?.Name ?? string.Empty;
    readonly InstanceId _originalInstanceId = component.InstanceId;
    InstanceId _activeInstanceId = component.InstanceId;

    public string Name => nameof(RemoveComponentCommand);
    public string Description => $"Remove {component.Typename?.Name}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _activeInstanceId is not null && !_activeInstanceId.IsEmpty() && _ownerId is not null && !_ownerId.IsEmpty() && !string.IsNullOrWhiteSpace(_componentTypeName);

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        => Task.FromResult(MauiProgram.GetService<EngineService>().RemoveComponent(_activeInstanceId) ? CommandResult.Success() : CommandResult.Failure());

    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();

        if (!world.TryGetGameObject(_ownerId, out var owner))
            return ValueTask.FromResult(CommandResult.Failure("Component owner was not found"));

        if (!engine.AddComponent(_ownerId, _componentTypeName, _originalInstanceId, out var restoredInstanceId))
            return ValueTask.FromResult(CommandResult.Failure("Restore component failed"));

        if (!restoredInstanceId.Equals(_originalInstanceId) || !world.TryGetComponent(restoredInstanceId, out var restored))
        {
            engine.RemoveComponent(restoredInstanceId);
            return ValueTask.FromResult(CommandResult.Failure("Restored component was not found"));
        }

        if (!engine.CommitChanges(restored.InstanceId, _beforeYaml))
        {
            engine.RemoveComponent(restored.InstanceId);
            return ValueTask.FromResult(CommandResult.Failure("Restore component state failed"));
        }

        if (!world.ApplyComponentYamlLocal(restored.InstanceId, _beforeYaml))
        {
            engine.RemoveComponent(restored.InstanceId);
            return ValueTask.FromResult(CommandResult.Failure("Refresh restored component state failed"));
        }

        _activeInstanceId = restoredInstanceId;
        return ValueTask.FromResult(CommandResult.Success());
    }
}

public sealed class ResetComponentToDefaultsCommand(Component component) : IUndoableEditorCommand
{
    readonly InstanceId _instanceId = component.InstanceId;
    readonly string _beforeYaml = EditorYaml.SerializeComponent(component);
    string? _afterYaml;
    public string Name => nameof(ResetComponentToDefaultsCommand);
    public string Description => $"Reset {component.Typename?.Name}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _instanceId is not null && !_instanceId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var ok = MauiProgram.GetService<EngineService>().ResetComponentToDefaults(_instanceId);
        if (ok && MauiProgram.GetService<WorldService>().TryGetComponent(_instanceId, out var refreshed))
            _afterYaml = EditorYaml.SerializeComponent(refreshed);
        return Task.FromResult(ok ? CommandResult.Success() : CommandResult.Failure());
    }
    public ValueTask<CommandResult> UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (!MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, _beforeYaml))
            return ValueTask.FromResult(CommandResult.Failure("Restore component state failed"));

        return ValueTask.FromResult(MauiProgram.GetService<WorldService>().ApplyComponentYamlLocal(_instanceId, _beforeYaml)
            ? CommandResult.Success()
            : CommandResult.Failure("Refresh restored component state failed"));
    }
}
