using CommunityToolkit.Mvvm.ComponentModel;
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
    CommandResult Apply(string yaml) => MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, yaml) ? CommandResult.Success() : CommandResult.Failure();
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
    CommandResult Apply(string yaml) => MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, yaml) ? CommandResult.Success() : CommandResult.Failure();
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
    readonly InstanceId _instanceId = gameObject.InstanceId;
    public string Name => nameof(DestroyGameObjectCommand);
    public string Description => $"Delete {gameObject.Name}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _instanceId is not null && !_instanceId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        => Task.FromResult(MauiProgram.GetService<EngineService>().DestroyObject(_instanceId) ? CommandResult.Success() : CommandResult.Failure("Destroy failed"));
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default) => ValueTask.CompletedTask;
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
        => Task.FromResult(MauiProgram.GetService<EngineService>().ReparentObject(_childId, _newParentId, keepWorldTransform) ? CommandResult.Success() : CommandResult.Failure());
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        MauiProgram.GetService<EngineService>().ReparentObject(_childId, _oldParentId, keepWorldTransform);
        return ValueTask.CompletedTask;
    }
}

public sealed class AddComponentCommand(GameObject gameObject, string componentTypeName) : IUndoableEditorCommand
{
    InstanceId? _componentId;
    public string Name => nameof(AddComponentCommand);
    public string Description => $"Add {componentTypeName}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => !string.IsNullOrWhiteSpace(componentTypeName);
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var world = MauiProgram.GetService<WorldService>();
        var before = world.GetComponents(gameObject).Select(x => x.InstanceId?.Value).ToHashSet();
        var ok = MauiProgram.GetService<EngineService>().AddComponent(gameObject.InstanceId, componentTypeName);
        if (!ok)
            return Task.FromResult(CommandResult.Failure());
        var created = world.GetComponents(gameObject).FirstOrDefault(x => x.InstanceId is not null && !before.Contains(x.InstanceId.Value));
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
    readonly InstanceId _instanceId = component.InstanceId;
    public string Name => nameof(RemoveComponentCommand);
    public string Description => $"Remove {component.Typename?.Name}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _instanceId is not null && !_instanceId.IsEmpty();
    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
        => Task.FromResult(MauiProgram.GetService<EngineService>().RemoveComponent(_instanceId) ? CommandResult.Success() : CommandResult.Failure());
    public ValueTask UndoAsync(ActionContext context, CancellationToken cancellationToken = default) => ValueTask.CompletedTask;
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
        MauiProgram.GetService<EngineService>().CommitChanges(_instanceId, _beforeYaml);
        return ValueTask.CompletedTask;
    }
}
