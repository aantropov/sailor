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

public sealed class UpdateGameObjectCommand(GameObject gameObject, string beforeYaml, string afterYaml, string description = "Edit object") : IUndoableEditorCommand
{
    readonly InstanceId _instanceId = gameObject.InstanceId;
    public string Name => nameof(UpdateGameObjectCommand);
    public string Description => description;
    public IHistoryMergePolicy? MergePolicy => new TimeWindowHistoryMergePolicy(TimeSpan.FromMilliseconds(750));
    public bool CanExecute(ActionContext context) => _instanceId is not null && !_instanceId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(Apply(afterYaml));
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default) { Apply(beforeYaml); return ValueTask.CompletedTask; }
    CommandResult Apply(string yaml)
    {
        if (!MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, yaml))
            return CommandResult.Failure();

        return MauiProgram.GetService<WorldService>().ApplyGameObjectYamlLocal(_instanceId, yaml)
            ? CommandResult.Success()
            : CommandResult.Failure();
    }
}

public sealed class UpdateComponentCommand(Component component, string beforeYaml, string afterYaml, string description = "Edit component") : IUndoableEditorCommand
{
    readonly InstanceId _instanceId = component.InstanceId;
    public string Name => nameof(UpdateComponentCommand);
    public string Description => description;
    public IHistoryMergePolicy? MergePolicy => new TimeWindowHistoryMergePolicy(TimeSpan.FromMilliseconds(750));
    public bool CanExecute(ActionContext context) => _instanceId is not null && !_instanceId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(Apply(afterYaml));
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default) { Apply(beforeYaml); return ValueTask.CompletedTask; }
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
        var before = world.Current.Prefabs.SelectMany(x => x.GameObjects).Select(x => x.InstanceId?.Value).ToHashSet();
        var ok = MauiProgram.GetService<EngineService>().CreateGameObject(parentId);
        if (!ok)
            return Task.FromResult(CommandResult.Failure());
        var created = world.Current.Prefabs.SelectMany(x => x.GameObjects).FirstOrDefault(x => x.InstanceId is not null && !before.Contains(x.InstanceId.Value));
        _createdId = created?.InstanceId;
        return Task.FromResult(CommandResult.Success(value: _createdId));
    }
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (_createdId is not null && !_createdId.IsEmpty())
            MauiProgram.GetService<EngineService>().DestroyObject(_createdId);
        return ValueTask.CompletedTask;
    }
}

public sealed class DestroyGameObjectCommand(GameObject gameObject) : IUndoableEditorCommand
{
    readonly InstanceId _originalInstanceId = gameObject.InstanceId;
    readonly string _name = gameObject.Name;
    readonly InstanceId? _parentId = gameObject.ParentIndex == uint.MaxValue
        ? null
        : MauiProgram.GetService<WorldService>().Current.Prefabs[gameObject.PrefabIndex].GameObjects[(int)gameObject.ParentIndex].InstanceId;
    readonly SailorEditor.ViewModels.PrefabFile? _prefabSnapshot = MauiProgram.GetService<AssetsService>().CreatePrefabAsset(null, gameObject);
    InstanceId _activeInstanceId = gameObject.InstanceId;

    public string Name => nameof(DestroyGameObjectCommand);
    public string Description => $"Delete {gameObject.Name}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _activeInstanceId is not null && !_activeInstanceId.IsEmpty() && _prefabSnapshot is not null;

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        => Task.FromResult(MauiProgram.GetService<EngineService>().DestroyObject(_activeInstanceId) ? CommandResult.Success() : CommandResult.Failure("Destroy failed"));

    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        var beforeIds = world.Current.Prefabs.SelectMany(x => x.GameObjects).Select(x => x.InstanceId?.Value).ToHashSet();

        if (_prefabSnapshot is null || !engine.InstantiatePrefab(_prefabSnapshot.FileId, _parentId))
            return ValueTask.CompletedTask;

        var restored = world.Current.Prefabs
            .SelectMany(x => x.GameObjects)
            .Where(x => x.InstanceId is not null && !beforeIds.Contains(x.InstanceId.Value))
            .FirstOrDefault(x => x.Name == _name && ((_parentId is null && x.ParentIndex == uint.MaxValue) || (_parentId is not null && x.ParentIndex != uint.MaxValue && world.Current.Prefabs[x.PrefabIndex].GameObjects[(int)x.ParentIndex].InstanceId == _parentId)))
            ?? world.Current.Prefabs.SelectMany(x => x.GameObjects).FirstOrDefault(x => x.InstanceId is not null && !beforeIds.Contains(x.InstanceId.Value));

        if (restored is not null)
            _activeInstanceId = restored.InstanceId;

        return ValueTask.CompletedTask;
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

    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        engine.ReparentObject(_childId, _oldParentId, keepWorldTransform);

        return ValueTask.CompletedTask;
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

        var before = world.GetComponents(owner).Select(x => x.InstanceId?.Value).ToHashSet();
        var ok = MauiProgram.GetService<EngineService>().AddComponent(_ownerId, componentTypeName);
        if (!ok)
            return Task.FromResult(CommandResult.Failure());

        if (!world.TryGetGameObject(_ownerId, out var refreshedOwner))
            return Task.FromResult(CommandResult.Success());

        var created = world.GetComponents(refreshedOwner).FirstOrDefault(x => x.InstanceId is not null && !before.Contains(x.InstanceId.Value));
        _componentId = created?.InstanceId;
        return Task.FromResult(CommandResult.Success(value: _componentId));
    }
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (_componentId is not null && !_componentId.IsEmpty())
            MauiProgram.GetService<EngineService>().RemoveComponent(_componentId);
        return ValueTask.CompletedTask;
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

    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();

        if (!world.TryGetGameObject(_ownerId, out var owner))
            return ValueTask.CompletedTask;

        var before = world.GetComponents(owner).Select(x => x.InstanceId?.Value).ToHashSet();
        if (!engine.AddComponent(_ownerId, _componentTypeName))
            return ValueTask.CompletedTask;

        if (!world.TryGetGameObject(_ownerId, out var refreshedOwner))
            return ValueTask.CompletedTask;

        var restored = world.GetComponents(refreshedOwner)
            .FirstOrDefault(x => x.InstanceId is not null && !before.Contains(x.InstanceId.Value));

        if (restored is null)
            return ValueTask.CompletedTask;

        if (!engine.CommitChanges(restored.InstanceId, _beforeYaml))
            return ValueTask.CompletedTask;

        if (world.TryGetComponent(_originalInstanceId, out var original))
        {
            _activeInstanceId = original.InstanceId;
            return ValueTask.CompletedTask;
        }

        if (world.TryGetComponent(restored.InstanceId, out var refreshed))
            _activeInstanceId = refreshed.InstanceId;

        return ValueTask.CompletedTask;
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
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        if (MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, _beforeYaml))
        {
            MauiProgram.GetService<WorldService>().ApplyComponentYamlLocal(_instanceId, _beforeYaml);
        }

        return ValueTask.CompletedTask;
    }
}
