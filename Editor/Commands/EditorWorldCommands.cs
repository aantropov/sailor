using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.History;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Numerics;
using System.Security.Cryptography;
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

public sealed class ApplyRuntimeSelectionCommand(InstanceId? instanceId) : IEditorCommand
{
    readonly InstanceId? _instanceId = instanceId;

    public string Name => nameof(ApplyRuntimeSelectionCommand);
    public bool CanExecute(ActionContext context) => true;

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        MauiProgram.GetService<SelectionService>().ApplyRuntimeSelection(_instanceId);
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
    public Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => ApplyAsync(_afterYaml, cancellationToken);
    public ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => new(ApplyAsync(_beforeYaml, cancellationToken));
    public bool CanCoalesceWith(IUndoableEditorCommand next) =>
        next is UpdateGameObjectCommand command && _instanceId.Equals(command._instanceId);
    public IUndoableEditorCommand CoalesceWith(IUndoableEditorCommand next)
    {
        var command = (UpdateGameObjectCommand)next;
        return new UpdateGameObjectCommand(_instanceId, _beforeYaml, command._afterYaml, command.Description);
    }
    async Task<CommandResult> ApplyAsync(
        string yaml,
        CancellationToken cancellationToken)
    {
        if (!await MauiProgram.GetService<EngineService>()
                .CommitChangesAsync(
                    _instanceId,
                    yaml,
                    cancellationToken))
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
    public Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => ApplyAsync(_afterYaml, cancellationToken);
    public ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => new(ApplyAsync(_beforeYaml, cancellationToken));
    public bool CanCoalesceWith(IUndoableEditorCommand next) =>
        next is UpdateComponentCommand command && _instanceId.Equals(command._instanceId);
    public IUndoableEditorCommand CoalesceWith(IUndoableEditorCommand next)
    {
        var command = (UpdateComponentCommand)next;
        return new UpdateComponentCommand(_instanceId, _beforeYaml, command._afterYaml, command.Description);
    }
    async Task<CommandResult> ApplyAsync(
        string yaml,
        CancellationToken cancellationToken)
    {
        if (!await MauiProgram.GetService<EngineService>()
                .CommitChangesAsync(
                    _instanceId,
                    yaml,
                    cancellationToken))
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
    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var world = MauiProgram.GetService<WorldService>();
        var engine = MauiProgram.GetService<EngineService>();
        var createdId = await engine.CreateGameObjectAsync(
            parentId,
            _createdId,
            cancellationToken);
        if (createdId is null)
            return CommandResult.Failure();

        _createdId = createdId;
        if (!world.TryGetGameObject(createdId, out _))
        {
            await engine.DestroyObjectAsync(
                createdId,
                CancellationToken.None);
            return CommandResult.Failure("Created object was not projected");
        }

        MauiProgram.GetService<SelectionService>().SelectInstance(createdId);

        return CommandResult.Success(value: _createdId);
    }
    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        if (_createdId is null || _createdId.IsEmpty())
            return CommandResult.Failure("Created object was not found");

        return await MauiProgram.GetService<EngineService>()
            .DestroyObjectAsync(_createdId, cancellationToken)
            ? CommandResult.Success()
            : CommandResult.Failure("Destroy created object failed");
    }
}

sealed class CreatedHierarchySnapshot
{
    readonly HashSet<string> _gameObjectIds;
    readonly HashSet<string> _componentIds;

    CreatedHierarchySnapshot(
        InstanceId rootInstanceId,
        string prefabYaml,
        HashSet<string> gameObjectIds,
        HashSet<string> componentIds,
        bool detachedFromPrefab,
        bool linkedPrefabSnapshot)
    {
        RootInstanceId = rootInstanceId;
        PrefabYaml = prefabYaml;
        _gameObjectIds = gameObjectIds;
        _componentIds = componentIds;
        DetachedFromPrefab = detachedFromPrefab;
        LinkedPrefabSnapshot = linkedPrefabSnapshot;
    }

    public InstanceId RootInstanceId { get; }
    public string PrefabYaml { get; }
    public bool DetachedFromPrefab { get; }
    public bool LinkedPrefabSnapshot { get; }

    public bool Contains(InstanceId? instanceId) =>
        instanceId is not null &&
        (_gameObjectIds.Contains(instanceId.Value) ||
            _componentIds.Contains(instanceId.Value));

    public bool IsAbsent(WorldService world) =>
        _gameObjectIds.All(value =>
            !world.TryGetGameObject(new InstanceId(value), out _)) &&
        _componentIds.All(value =>
            !world.TryGetComponent(new InstanceId(value), out _));

    public bool IsProjected(WorldService world) =>
        _gameObjectIds.All(value =>
            world.TryGetGameObject(new InstanceId(value), out _)) &&
        _componentIds.All(value =>
            world.TryGetComponent(new InstanceId(value), out _));

    public AuthoritativeHierarchyProjection ClassifyProjection(
        WorldService world,
        InstanceId? expectedParentId)
    {
        if (IsAbsent(world))
        {
            return AuthoritativeHierarchyProjection.Absent;
        }

        return MatchesProjection(world, expectedParentId)
            ? AuthoritativeHierarchyProjection.Exact
            : AuthoritativeHierarchyProjection.Mismatch;
    }

    public bool MatchesProjection(
        WorldService world,
        InstanceId? expectedParentId)
    {
        if (!IsProjected(world) ||
            !world.TryGetGameObject(RootInstanceId, out var root) ||
            !SameInstanceId(
                world.ResolveParentInstanceId(root),
                expectedParentId))
        {
            return false;
        }

        try
        {
            var projectedPrefab =
                CreateSnapshotPrefab(world, root);
            return string.Equals(
                EditorYaml.SerializePrefab(projectedPrefab),
                PrefabYaml,
                StringComparison.Ordinal);
        }
        catch
        {
            return false;
        }
    }

    public static bool TryCapture(
        WorldService world,
        GameObject root,
        out CreatedHierarchySnapshot? snapshot,
        out string diagnostic)
    {
        snapshot = null;
        diagnostic = string.Empty;

        try
        {
            var prefab = CreateSnapshotPrefab(world, root);
            var gameObjectIds = prefab.GameObjects
                .Where(gameObject => gameObject.InstanceId is not null)
                .Select(gameObject => gameObject.InstanceId.Value)
                .Where(value => !string.IsNullOrWhiteSpace(value))
                .ToHashSet(StringComparer.Ordinal);
            var componentIds = prefab.Components
                .Where(component => component.InstanceId is not null)
                .Select(component => component.InstanceId.Value)
                .Where(value => !string.IsNullOrWhiteSpace(value))
                .ToHashSet(StringComparer.Ordinal);

            if (gameObjectIds.Count != prefab.GameObjects.Count ||
                componentIds.Count != prefab.Components.Count ||
                root.InstanceId is null ||
                root.InstanceId.IsEmpty() ||
                !gameObjectIds.Contains(root.InstanceId.Value))
            {
                diagnostic =
                    "Created hierarchy contains invalid or duplicate instance ids";
                return false;
            }

            snapshot = new CreatedHierarchySnapshot(
                new InstanceId(root.InstanceId.Value),
                EditorYaml.SerializePrefab(prefab),
                gameObjectIds,
                componentIds,
                prefab.DetachedFromPrefab,
                prefab.LinkedPrefabSnapshot);
            return true;
        }
        catch (Exception exception)
        {
            diagnostic = exception.Message;
            return false;
        }
    }

    static bool SameInstanceId(InstanceId? left, InstanceId? right)
    {
        var leftValue = left is null || left.IsEmpty()
            ? null
            : left.Value;
        var rightValue = right is null || right.IsEmpty()
            ? null
            : right.Value;
        return string.Equals(
            leftValue,
            rightValue,
            StringComparison.Ordinal);
    }

    static Prefab CreateSnapshotPrefab(
        WorldService world,
        GameObject root)
    {
        var prefab = world.CreatePrefabFromSubHierarchy(root, out _);
        var parentId = world.ResolveParentInstanceId(root);
        if (root.ParentIndex == uint.MaxValue &&
            root.PrefabIndex >= 0 &&
            root.PrefabIndex < world.Current.Prefabs.Count)
        {
            var linkedRootPrefab =
                world.Current.Prefabs[root.PrefabIndex];
            if (linkedRootPrefab.FileId is not null &&
                !linkedRootPrefab.FileId.IsEmpty() &&
                ContainsMappedInstanceId(
                    linkedRootPrefab,
                    root.InstanceId))
            {
                prefab.FileId =
                    (FileId)linkedRootPrefab.FileId.Clone();
                prefab.ParentInstanceId = parentId?.Value;
                prefab.InstanceIds = linkedRootPrefab.InstanceIds is null
                    ? null
                    : new Dictionary<string, string>(
                        linkedRootPrefab.InstanceIds,
                        StringComparer.Ordinal);
                prefab.GameObjectOverrides =
                    linkedRootPrefab.GameObjectOverrides?
                        .ToDictionary(
                            entry => entry.Key,
                            entry =>
                                (PrefabGameObjectOverride)
                                    entry.Value.Clone(),
                            StringComparer.Ordinal);
                prefab.ComponentOverrides =
                    linkedRootPrefab.ComponentOverrides?
                        .ToDictionary(
                            entry => entry.Key,
                            entry =>
                                (PrefabComponentOverride)
                                    entry.Value.Clone(),
                            StringComparer.Ordinal);
                prefab.LinkedPrefabSnapshot = true;
                return prefab;
            }
        }

        if (parentId is null ||
            !world.TryGetGameObject(parentId, out var parent) ||
            parent.PrefabIndex < 0 ||
            parent.PrefabIndex >= world.Current.Prefabs.Count)
        {
            return prefab;
        }

        var linkedParentPrefab =
            world.Current.Prefabs[parent.PrefabIndex];
        if (linkedParentPrefab.FileId is null ||
            linkedParentPrefab.FileId.IsEmpty() ||
            !ContainsMappedInstanceId(linkedParentPrefab, parentId) ||
            ContainsMappedInstanceId(
                linkedParentPrefab,
                root.InstanceId))
        {
            return prefab;
        }

        prefab.DetachedFromPrefab = true;
        prefab.ParentInstanceId = parentId.Value;
        return prefab;
    }

    static bool ContainsMappedInstanceId(
        Prefab prefab,
        InstanceId? instanceId)
        => instanceId is not null &&
            !instanceId.IsEmpty() &&
            prefab.InstanceIds?.Values.Any(value =>
                string.Equals(
                    value,
                    instanceId.Value,
                    StringComparison.Ordinal)) == true;
}

static class AuthoritativeHierarchyReconciliation
{
    public static async Task<(
        AuthoritativeHierarchyProjection Projection,
        string? Diagnostic)> RefreshAndClassifyAsync(
        EngineService engine,
        WorldService world,
        CreatedHierarchySnapshot snapshot,
        InstanceId? parentId)
    {
        try
        {
            if (!await engine.RefreshCurrentWorldAuthoritativelyAsync(
                    CancellationToken.None))
            {
                return (
                    AuthoritativeHierarchyProjection.Unavailable,
                    "authoritative refresh was not published");
            }

            return (
                snapshot.ClassifyProjection(world, parentId),
                null);
        }
        catch (Exception exception)
        {
            return (
                AuthoritativeHierarchyProjection.Unavailable,
                exception.Message);
        }
    }

    public static async Task<(
        bool Available,
        bool RootAbsent,
        string? Diagnostic)> RefreshAndCheckRootAbsentAsync(
        EngineService engine,
        WorldService world,
        InstanceId rootInstanceId)
    {
        try
        {
            if (!await engine.RefreshCurrentWorldAuthoritativelyAsync(
                    CancellationToken.None))
            {
                return (
                    false,
                    false,
                    "authoritative refresh was not published");
            }

            return (
                true,
                !world.TryGetGameObject(rootInstanceId, out _),
                null);
        }
        catch (Exception exception)
        {
            return (false, false, exception.Message);
        }
    }
}

static class OwnedHierarchyRollback
{
    public static async Task<(
        bool ConfirmedAbsent,
        string? Diagnostic)> EnsureAbsentAsync(
        EngineService engine,
        WorldService world,
        InstanceId rootInstanceId)
    {
        var probe =
            await AuthoritativeHierarchyReconciliation
                .RefreshAndCheckRootAbsentAsync(
                    engine,
                    world,
                    rootInstanceId);
        if (probe.Available && probe.RootAbsent)
        {
            return (true, null);
        }

        var diagnostic = probe.Diagnostic;
        for (var attempt = 0; attempt < 2; attempt++)
        {
            try
            {
                if (!await engine.RequestDestroyObjectAsync(
                        rootInstanceId,
                        CancellationToken.None))
                {
                    diagnostic =
                        "the engine rejected the exact-root destroy request";
                }
            }
            catch (Exception exception)
            {
                diagnostic = exception.Message;
            }

            probe =
                await AuthoritativeHierarchyReconciliation
                    .RefreshAndCheckRootAbsentAsync(
                        engine,
                        world,
                        rootInstanceId);
            if (probe.Available && probe.RootAbsent)
            {
                return (true, null);
            }

            diagnostic ??= probe.Diagnostic;
        }

        return (false, diagnostic);
    }

    public static async Task<CommandResult> FailureAsync(
        EngineService engine,
        WorldService world,
        InstanceId rootInstanceId,
        string failureMessage)
    {
        var cleanup = await EnsureAbsentAsync(
            engine,
            world,
            rootInstanceId);
        if (cleanup.ConfirmedAbsent)
        {
            return CommandResult.Failure(failureMessage);
        }

        return CommandResult.Failure(
            string.IsNullOrWhiteSpace(cleanup.Diagnostic)
                ? $"{failureMessage}; exact-root rollback could not be confirmed"
                : $"{failureMessage}; exact-root rollback could not be confirmed ({cleanup.Diagnostic})");
    }
}

static class StrictHierarchyRestore
{
    public static async Task<CommandResult> RestoreAsync(
        CreatedHierarchySnapshot snapshot,
        InstanceId? parentId,
        FileId? prefabLinkFileId,
        string failureMessage)
    {
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();

        try
        {
            if (!await engine.RefreshCurrentWorldAuthoritativelyAsync(
                    CancellationToken.None))
            {
                return CommandResult.Failure(
                    $"{failureMessage}: authoritative preflight refresh failed");
            }
        }
        catch (Exception exception)
        {
            return CommandResult.Failure(
                $"{failureMessage}: authoritative preflight refresh failed: {exception.Message}");
        }

        if (snapshot.ClassifyProjection(world, parentId) !=
            AuthoritativeHierarchyProjection.Absent)
        {
            return CommandResult.Failure(
                $"{failureMessage}: saved instance ids are already in use");
        }

        // EditorCommandDispatcher serializes managed mutations. Once the
        // authoritative preflight proves every saved id absent, a full
        // post-request snapshot match is ownership proof for this operation.
        var response = EngineMutationResponseState.Unknown;
        string? requestDiagnostic = null;
        try
        {
            response =
                await engine.RequestInstantiatePrefabFromYamlStrictAsync(
                    snapshot.PrefabYaml,
                    parentId,
                    CancellationToken.None)
                ? EngineMutationResponseState.Accepted
                : EngineMutationResponseState.Rejected;
        }
        catch (Exception exception)
        {
            requestDiagnostic = exception.Message;
        }

        var projection = AuthoritativeHierarchyProjection.Unavailable;
        string? refreshDiagnostic = null;
        try
        {
            if (await engine.RefreshCurrentWorldAuthoritativelyAsync(
                    CancellationToken.None))
            {
                projection = snapshot.ClassifyProjection(
                    world,
                    parentId);
            }
        }
        catch (Exception exception)
        {
            refreshDiagnostic = exception.Message;
        }

        var resolution =
            HierarchyMutationRecoveryPolicy.ResolveStrictRestore(
                response,
                projection);
        if (resolution.RollbackOwnedHierarchy)
        {
            return await OwnedHierarchyRollback.FailureAsync(
                engine,
                world,
                snapshot.RootInstanceId,
                BuildFailureMessage(
                    failureMessage,
                    "restored hierarchy did not match the saved snapshot",
                    requestDiagnostic,
                    refreshDiagnostic));
        }

        if (!resolution.Succeeded)
        {
            return CommandResult.Failure(
                BuildFailureMessage(
                    failureMessage,
                    resolution.OutcomeUncertain
                        ? "restore outcome is uncertain; no rollback was performed because ownership was not proven"
                        : "restore was not applied",
                    requestDiagnostic,
                    refreshDiagnostic));
        }

        if (prefabLinkFileId is not null)
        {
            try
            {
                var linked = await engine.SetPrefabLinkAsync(
                    snapshot.RootInstanceId,
                    prefabLinkFileId,
                    CancellationToken.None);
                if ((!linked ||
                        !IsLinkedToPrefab(
                            world,
                            snapshot.RootInstanceId,
                            prefabLinkFileId)) &&
                    (!await TryRefreshAuthoritativeAsync(engine) ||
                        !IsLinkedToPrefab(
                            world,
                            snapshot.RootInstanceId,
                            prefabLinkFileId)))
                {
                    return await OwnedHierarchyRollback.FailureAsync(
                        engine,
                        world,
                        snapshot.RootInstanceId,
                        $"{failureMessage}: prefab link restore failed");
                }
            }
            catch (Exception exception)
            {
                if (await TryRefreshAuthoritativeAsync(engine) &&
                    IsLinkedToPrefab(
                        world,
                        snapshot.RootInstanceId,
                        prefabLinkFileId))
                {
                    return CommandResult.Success(
                        "Strict hierarchy restore was reconciled after a lost prefab-link response.",
                        snapshot.RootInstanceId);
                }

                return await OwnedHierarchyRollback.FailureAsync(
                    engine,
                    world,
                    snapshot.RootInstanceId,
                    $"{failureMessage}: prefab link restore failed unexpectedly: {exception.Message}");
            }
        }

        return CommandResult.Success(
            resolution.OutcomeUncertain
                ? "Strict hierarchy restore was reconciled from the authoritative full snapshot."
                : null,
            snapshot.RootInstanceId);
    }

    static async Task<bool> TryRefreshAuthoritativeAsync(
        EngineService engine)
    {
        try
        {
            return await engine.RefreshCurrentWorldAuthoritativelyAsync(
                CancellationToken.None);
        }
        catch
        {
            return false;
        }
    }

    static bool IsLinkedToPrefab(
        WorldService world,
        InstanceId rootInstanceId,
        FileId prefabFileId)
    {
        if (!world.TryGetGameObject(rootInstanceId, out var root) ||
            root.PrefabIndex < 0 ||
            root.PrefabIndex >= world.Current.Prefabs.Count)
        {
            return false;
        }

        var projectedPrefab = world.Current.Prefabs[root.PrefabIndex];
        return projectedPrefab.FileId is not null &&
            projectedPrefab.FileId.Equals(prefabFileId);
    }

    static string BuildFailureMessage(
        string failureMessage,
        string detail,
        string? requestDiagnostic,
        string? refreshDiagnostic)
    {
        var diagnostics = new List<string>();
        if (!string.IsNullOrWhiteSpace(requestDiagnostic))
        {
            diagnostics.Add($"request: {requestDiagnostic}");
        }
        if (!string.IsNullOrWhiteSpace(refreshDiagnostic))
        {
            diagnostics.Add($"refresh: {refreshDiagnostic}");
        }

        return diagnostics.Count == 0
            ? $"{failureMessage}: {detail}"
            : $"{failureMessage}: {detail} ({string.Join("; ", diagnostics)})";
    }
}

sealed class CreatedHierarchyCommandState(
    InstanceId? parentId,
    FileId? prefabLinkFileId = null)
{
    readonly InstanceId? _parentId = parentId is null ||
        parentId.IsEmpty()
        ? null
        : new InstanceId(parentId.Value);
    readonly FileId? _prefabLinkFileId = prefabLinkFileId is null ||
        prefabLinkFileId.IsEmpty()
        ? null
        : new FileId(prefabLinkFileId.Value);
    CreatedHierarchySnapshot? _snapshot;

    public async Task<CommandResult> ExecuteAsync(
        Func<Task<InstanceId?>> createAsync,
        string createFailureMessage,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();

        if (_snapshot is not null)
        {
            return await RestoreAsync(createFailureMessage);
        }

        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        var createdId = await createAsync();
        if (createdId is null || createdId.IsEmpty())
        {
            return CommandResult.Failure(createFailureMessage);
        }

        if (!world.TryGetGameObject(createdId, out var root))
        {
            return await OwnedHierarchyRollback.FailureAsync(
                engine,
                world,
                createdId,
                $"{createFailureMessage}: created hierarchy was not projected");
        }

        if (!CreatedHierarchySnapshot.TryCapture(
                world,
                root,
                out var snapshot,
                out var diagnostic) ||
            snapshot is null)
        {
            return await OwnedHierarchyRollback.FailureAsync(
                engine,
                world,
                createdId,
                string.IsNullOrWhiteSpace(diagnostic)
                    ? $"{createFailureMessage}: snapshot failed"
                    : $"{createFailureMessage}: {diagnostic}");
        }

        _snapshot = snapshot;
        try
        {
            MauiProgram.GetService<SelectionService>()
                .SelectInstance(snapshot.RootInstanceId);
        }
        catch (Exception exception)
        {
            return CommandResult.Success(
                $"Hierarchy was created, but editor selection failed: {exception.Message}",
                snapshot.RootInstanceId);
        }

        return CommandResult.Success(value: snapshot.RootInstanceId);
    }

    public async ValueTask<CommandResult> UndoAsync(
        CancellationToken cancellationToken)
    {
        if (_snapshot is null)
        {
            return CommandResult.Failure(
                "Created hierarchy snapshot was not found");
        }

        cancellationToken.ThrowIfCancellationRequested();

        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        var preflight =
            await AuthoritativeHierarchyReconciliation
                .RefreshAndClassifyAsync(
                    engine,
                    world,
                    _snapshot,
                    _parentId);
        if (preflight.Projection ==
            AuthoritativeHierarchyProjection.Unavailable)
        {
            return CommandResult.Failure(
                $"Destroy created hierarchy failed: authoritative preflight refresh failed{FormatDiagnostic(preflight.Diagnostic)}");
        }
        if (preflight.Projection ==
            AuthoritativeHierarchyProjection.Mismatch)
        {
            return CommandResult.Failure(
                "Destroy created hierarchy failed: the authoritative hierarchy no longer matches the saved snapshot");
        }
        if (preflight.Projection ==
            AuthoritativeHierarchyProjection.Absent)
        {
            ClearSelectionIfOwned(_snapshot);
            return CommandResult.Success(
                "Created hierarchy was already absent; undo reconciled without another mutation.");
        }

        var response = EngineMutationResponseState.Unknown;
        string? requestDiagnostic = null;
        try
        {
            response = await engine.RequestDestroyObjectAsync(
                    _snapshot.RootInstanceId,
                    CancellationToken.None)
                ? EngineMutationResponseState.Accepted
                : EngineMutationResponseState.Rejected;
        }
        catch (Exception exception)
        {
            requestDiagnostic = exception.Message;
        }

        var postMutation =
            await AuthoritativeHierarchyReconciliation
                .RefreshAndClassifyAsync(
                    engine,
                    world,
                    _snapshot,
                    _parentId);
        if (postMutation.Projection !=
            AuthoritativeHierarchyProjection.Absent)
        {
            var responseDescription = response switch
            {
                EngineMutationResponseState.Accepted =>
                    "the engine accepted destroy",
                EngineMutationResponseState.Rejected =>
                    "the engine rejected destroy",
                _ => "the destroy response was lost"
            };
            return CommandResult.Failure(
                $"Destroy created hierarchy could not be confirmed absent ({responseDescription})" +
                FormatDiagnostics(
                    requestDiagnostic,
                    postMutation.Diagnostic));
        }

        ClearSelectionIfOwned(_snapshot);
        return CommandResult.Success(
            response == EngineMutationResponseState.Accepted
                ? null
                : "Destroy created hierarchy was reconciled from the authoritative world snapshot.");
    }

    async Task<CommandResult> RestoreAsync(string createFailureMessage)
    {
        var snapshot = _snapshot!;
        var result = await StrictHierarchyRestore.RestoreAsync(
            snapshot,
            _parentId,
            snapshot.DetachedFromPrefab ||
                snapshot.LinkedPrefabSnapshot
                ? null
                : _prefabLinkFileId,
            createFailureMessage);
        if (result.Succeeded)
        {
            try
            {
                MauiProgram.GetService<SelectionService>()
                    .SelectInstance(snapshot.RootInstanceId);
            }
            catch (Exception exception)
            {
                return CommandResult.Success(
                    string.IsNullOrWhiteSpace(result.Message)
                        ? $"Hierarchy was restored, but editor selection failed: {exception.Message}"
                        : $"{result.Message} Editor selection failed: {exception.Message}",
                    result.Value);
            }
        }

        return result;
    }

    static void ClearSelectionIfOwned(
        CreatedHierarchySnapshot snapshot)
    {
        var selection = MauiProgram.GetService<SelectionService>();
        if (snapshot.Contains(selection.SelectedInstanceId))
        {
            selection.ClearSelection();
        }
    }

    static string FormatDiagnostic(string? diagnostic) =>
        string.IsNullOrWhiteSpace(diagnostic)
            ? string.Empty
            : $": {diagnostic}";

    static string FormatDiagnostics(
        string? requestDiagnostic,
        string? refreshDiagnostic)
    {
        var diagnostics = new List<string>();
        if (!string.IsNullOrWhiteSpace(requestDiagnostic))
        {
            diagnostics.Add($"request: {requestDiagnostic}");
        }
        if (!string.IsNullOrWhiteSpace(refreshDiagnostic))
        {
            diagnostics.Add($"refresh: {refreshDiagnostic}");
        }

        return diagnostics.Count == 0
            ? string.Empty
            : $" ({string.Join("; ", diagnostics)})";
    }

}

public sealed class CreateModelGameObjectCommand(
    AssetFile modelFile,
    string objectName,
    GameObject? parent = null,
    Vec4? worldPosition = null) : IUndoableEditorCommand
{
    const string MeshRendererComponentTypeName =
        "Sailor::MeshRendererComponent";
    const string ModelPropertyName = "model";
    const float WorldPositionEpsilon = 0.001f;

    readonly FileId _modelFileId = modelFile?.FileId;
    readonly InstanceId? _parentId = parent?.InstanceId;
    readonly Vec4? _worldPosition = worldPosition;
    readonly string _objectName = objectName;
    readonly InstanceId _ownedGameObjectId =
        CreateCanonicalGameObjectInstanceId();
    readonly CreatedHierarchyCommandState _state = new(
        parent?.InstanceId);

    public string Name => nameof(CreateModelGameObjectCommand);
    public string Description => $"Create {_objectName}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) =>
        _modelFileId is not null &&
        !_modelFileId.IsEmpty() &&
        !string.IsNullOrWhiteSpace(_objectName);

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        return await _state.ExecuteAsync(
            CreateWithGenericOperationsAsync,
            "Create model GameObject failed",
            cancellationToken);
    }

    public ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => _state.UndoAsync(cancellationToken);

    async Task<InstanceId?> CreateWithGenericOperationsAsync()
    {
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        var ownedComponentId =
            CreateCanonicalComponentInstanceId(
                _ownedGameObjectId);
        var creationSubmitted = false;
        string? failureDiagnostic = null;

        try
        {
            var preflight =
                await AuthoritativeHierarchyReconciliation
                    .RefreshAndCheckRootAbsentAsync(
                        engine,
                        world,
                        _ownedGameObjectId);
            if (!preflight.Available || !preflight.RootAbsent)
            {
                Console.Error.WriteLine(
                    "Create model GameObject preflight could not prove the owned instance id absent" +
                    FormatDiagnostic(preflight.Diagnostic));
                return null;
            }

            var initialParentId = _worldPosition is null
                ? _parentId
                : null;

            creationSubmitted = true;
            var createdId = await engine.CreateGameObjectAsync(
                initialParentId,
                _ownedGameObjectId,
                CancellationToken.None);
            if (!SameInstanceId(
                    createdId,
                    _ownedGameObjectId) ||
                !world.TryGetGameObject(
                    _ownedGameObjectId,
                    out _))
            {
                throw new InvalidOperationException(
                    "CreateGameObject did not project the owned GameObject");
            }

            var componentId = await engine.AddComponentAsync(
                _ownedGameObjectId,
                MeshRendererComponentTypeName,
                ownedComponentId,
                CancellationToken.None);
            if (!SameInstanceId(
                    componentId,
                    ownedComponentId) ||
                !world.TryGetGameObject(
                    _ownedGameObjectId,
                    out var createdObject) ||
                !world.TryGetComponent(
                    ownedComponentId,
                    out var meshRenderer))
            {
                throw new InvalidOperationException(
                    "AddComponent did not project the owned MeshRenderer");
            }

            var updatedObject = CloneGameObject(createdObject);
            updatedObject.Name = _objectName;
            if (_worldPosition is not null)
            {
                updatedObject.Position = new Vec4(_worldPosition);
            }

            if (!await engine.CommitChangesAsync(
                    _ownedGameObjectId,
                    EditorYaml.SerializeGameObject(updatedObject),
                    CancellationToken.None))
            {
                throw new InvalidOperationException(
                    "ChangeValue failed for GameObject");
            }

            var updatedMeshRenderer = CloneComponent(meshRenderer);
            if (!updatedMeshRenderer.OverrideProperties.ContainsKey(
                    ModelPropertyName))
            {
                throw new InvalidOperationException(
                    "MeshRenderer has no reflected model property");
            }

            updatedMeshRenderer.OverrideProperties[ModelPropertyName] =
                new ObjectPtr
                {
                    FileId = new FileId(_modelFileId.Value),
                    InstanceId = new InstanceId(
                        InstanceId.NullInstanceId)
                };
            if (!await engine.CommitChangesAsync(
                    ownedComponentId,
                    EditorYaml.SerializeComponent(
                        updatedMeshRenderer),
                    CancellationToken.None))
            {
                throw new InvalidOperationException(
                    "ChangeValue failed for MeshRenderer.model");
            }

            if (_worldPosition is not null &&
                _parentId is not null &&
                !_parentId.IsEmpty() &&
                !await engine.ReparentObjectAsync(
                    _ownedGameObjectId,
                    _parentId,
                    keepWorldTransform: true,
                    CancellationToken.None))
            {
                throw new InvalidOperationException(
                    "ReparentObject failed");
            }

            if (!await engine.RefreshCurrentWorldAuthoritativelyAsync(
                    CancellationToken.None) ||
                !MatchesFinalProjection(
                    world,
                    ownedComponentId))
            {
                throw new InvalidOperationException(
                    "The authoritative result did not match the requested name, parent, world position, component, and model");
            }

            return _ownedGameObjectId;
        }
        catch (Exception exception)
        {
            failureDiagnostic = exception.Message;
        }

        if (!creationSubmitted)
        {
            Console.Error.WriteLine(
                $"Create model GameObject failed: {failureDiagnostic}");
            return null;
        }

        var cleanup = await OwnedHierarchyRollback.EnsureAbsentAsync(
            engine,
            world,
            _ownedGameObjectId);
        if (!cleanup.ConfirmedAbsent ||
            world.TryGetComponent(ownedComponentId, out _))
        {
            throw new InvalidOperationException(
                $"Create model GameObject failed ({failureDiagnostic}); rollback could not confirm owned GameObject/component absent for '{_ownedGameObjectId.Value}'" +
                FormatDiagnostic(cleanup.Diagnostic));
        }

        Console.Error.WriteLine(
            $"Create model GameObject failed and was rolled back: {failureDiagnostic}");
        return null;
    }

    bool MatchesFinalProjection(
        WorldService world,
        InstanceId componentId)
    {
        if (!world.TryGetGameObject(
                _ownedGameObjectId,
                out var gameObject) ||
            !string.Equals(
                gameObject.Name,
                _objectName,
                StringComparison.Ordinal) ||
            !SameInstanceId(
                world.ResolveParentInstanceId(gameObject),
                _parentId) ||
            !world.TryGetComponent(
                componentId,
                out var meshRenderer) ||
            !string.Equals(
                meshRenderer.Typename?.Name,
                MeshRendererComponentTypeName,
                StringComparison.Ordinal) ||
            !SameInstanceId(
                world.FindOwner(meshRenderer)?.InstanceId,
                _ownedGameObjectId))
        {
            return false;
        }

        var projectedComponents =
            world.GetComponents(gameObject);
        if (projectedComponents.Count != 1 ||
            !SameInstanceId(
                projectedComponents[0].InstanceId,
                componentId) ||
            !meshRenderer.OverrideProperties.TryGetValue(
                ModelPropertyName,
                out var modelValue) ||
            modelValue is not ObjectPtr model ||
            model.FileId is null ||
            !string.Equals(
                model.FileId.Value,
                _modelFileId.Value,
                StringComparison.Ordinal) ||
            (model.InstanceId is not null &&
                !model.InstanceId.IsEmpty()))
        {
            return false;
        }

        if (_worldPosition is null)
        {
            return IsNear(
                ToVector3(gameObject.Position),
                Vector3.Zero);
        }

        return TryResolveWorldPosition(
                world,
                gameObject,
                out var projectedWorldPosition) &&
            IsNear(
                projectedWorldPosition,
                ToVector3(_worldPosition));
    }

    static bool TryResolveWorldPosition(
        WorldService world,
        GameObject gameObject,
        out Vector3 worldPosition)
    {
        worldPosition = ToVector3(gameObject.Position);
        var current = gameObject;
        var visited = new HashSet<string>(
            StringComparer.Ordinal)
        {
            gameObject.InstanceId.Value
        };

        while (world.ResolveParentInstanceId(current) is
            { } parentInstanceId)
        {
            if (!visited.Add(parentInstanceId.Value) ||
                !world.TryGetGameObject(
                    parentInstanceId,
                    out var parent) ||
                parent.Position is null ||
                parent.Rotation is null ||
                parent.Scale is null)
            {
                return false;
            }

            worldPosition *= new Vector3(
                parent.Scale.X,
                parent.Scale.Y,
                parent.Scale.Z);
            worldPosition = Vector3.Transform(
                    worldPosition,
                    ToQuaternion(parent.Rotation)) +
                ToVector3(parent.Position);
            current = parent;
        }

        return IsFinite(worldPosition);
    }

    static Quaternion ToQuaternion(Rotation rotation)
    {
        var quat = rotation?.Quat ??
            Quat.FromYawPitchRoll(
                rotation?.Yaw ?? 0.0f,
                rotation?.Pitch ?? 0.0f,
                rotation?.Roll ?? 0.0f);
        var result = new Quaternion(
            quat.X,
            quat.Y,
            quat.Z,
            quat.W);
        return result.LengthSquared() > 0.0f &&
            IsFinite(result)
            ? Quaternion.Normalize(result)
            : Quaternion.Identity;
    }

    static GameObject CloneGameObject(GameObject gameObject) => new()
    {
        Name = gameObject.Name,
        InstanceId = new InstanceId(gameObject.InstanceId.Value),
        ParentIndex = gameObject.ParentIndex,
        Position = new Vec4(gameObject.Position),
        Rotation = new Rotation(gameObject.Rotation),
        Scale = new Vec4(gameObject.Scale),
        ComponentIndices = [.. gameObject.ComponentIndices]
    };

    static Component CloneComponent(Component component)
    {
        var yaml = EditorYaml.SerializeComponent(component);
        return SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new ComponentYamlConverter())
            .Build()
            .Deserialize<Component>(yaml);
    }

    static Vector3 ToVector3(Vec4 value) =>
        value is null
            ? new Vector3(
                float.NaN,
                float.NaN,
                float.NaN)
            : new Vector3(
                value.X,
                value.Y,
                value.Z);

    static bool IsNear(
        Vector3 actual,
        Vector3 expected)
    {
        return IsFinite(actual) &&
            IsFinite(expected) &&
            NearlyEqual(actual.X, expected.X) &&
            NearlyEqual(actual.Y, expected.Y) &&
            NearlyEqual(actual.Z, expected.Z);
    }

    static bool NearlyEqual(
        float actual,
        float expected)
    {
        var magnitude = MathF.Max(
            1.0f,
            MathF.Max(
                MathF.Abs(actual),
                MathF.Abs(expected)));
        return MathF.Abs(actual - expected) <=
            WorldPositionEpsilon * magnitude;
    }

    static bool IsFinite(Vector3 value) =>
        float.IsFinite(value.X) &&
        float.IsFinite(value.Y) &&
        float.IsFinite(value.Z);

    static bool IsFinite(Quaternion value) =>
        float.IsFinite(value.X) &&
        float.IsFinite(value.Y) &&
        float.IsFinite(value.Z) &&
        float.IsFinite(value.W);

    static bool SameInstanceId(
        InstanceId? left,
        InstanceId? right)
    {
        var leftValue = left is null || left.IsEmpty()
            ? null
            : left.Value;
        var rightValue = right is null || right.IsEmpty()
            ? null
            : right.Value;
        return string.Equals(
            leftValue,
            rightValue,
            StringComparison.Ordinal);
    }

    static string FormatDiagnostic(string? diagnostic) =>
        string.IsNullOrWhiteSpace(diagnostic)
            ? string.Empty
            : $": {diagnostic}";

    static InstanceId CreateCanonicalGameObjectInstanceId() =>
        new(Convert.ToHexString(
            RandomNumberGenerator.GetBytes(10)));

    static InstanceId CreateCanonicalComponentInstanceId(
        InstanceId owner) =>
        new(
            $"{Convert.ToHexString(RandomNumberGenerator.GetBytes(8))}_{owner.Value}");
}

public sealed class InstantiatePrefabAssetCommand(
    AssetFile prefabFile,
    GameObject? parent = null,
    Vec4? worldPosition = null) : IUndoableEditorCommand
{
    readonly FileId _prefabFileId = prefabFile?.FileId;
    readonly InstanceId? _parentId = parent?.InstanceId;
    readonly Vec4? _worldPosition = worldPosition;
    readonly CreatedHierarchyCommandState _state = new(
        parent?.InstanceId,
        prefabFile?.FileId);

    public string Name => nameof(InstantiatePrefabAssetCommand);
    public string Description => "Instantiate Prefab";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) =>
        _prefabFileId is not null && !_prefabFileId.IsEmpty();

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        return await _state.ExecuteAsync(
            () => engine.InstantiatePrefabInstanceAsync(
                _prefabFileId,
                _parentId,
                _worldPosition,
                CancellationToken.None),
            "Instantiate prefab failed",
            cancellationToken);
    }

    public ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => _state.UndoAsync(cancellationToken);
}

public sealed class FocusEditorCameraCommand(InstanceId instanceId) : IEditorCommand
{
    public string Name => nameof(FocusEditorCameraCommand);
    public bool CanExecute(ActionContext context) =>
        instanceId is not null && !instanceId.IsEmpty();

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => await MauiProgram.GetService<EngineService>()
            .FocusEditorCameraAsync(instanceId, cancellationToken)
            ? CommandResult.Success()
            : CommandResult.Failure("Focus editor camera failed");
}

public sealed class BreakPrefabLinkCommand(
    InstanceId instanceId,
    FileId prefabFileId) : IUndoableEditorCommand
{
    public string Name => nameof(BreakPrefabLinkCommand);
    public string Description => "Break Prefab Link";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) =>
        instanceId is not null &&
        !instanceId.IsEmpty() &&
        prefabFileId is not null &&
        !prefabFileId.IsEmpty();

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        try
        {
            if (!await engine.BreakPrefabLinkAsync(
                    instanceId,
                    CancellationToken.None))
            {
                return CommandResult.Failure("Break prefab link failed");
            }

            if (IsProjectedUnlinked(world, instanceId))
            {
                return CommandResult.Success();
            }

            var restored = await RestoreLinkAsync(
                engine,
                world,
                instanceId,
                prefabFileId);
            return CommandResult.Failure(
                restored
                    ? "Break prefab link was not reflected in the world projection; the original link was restored."
                    : "Break prefab link was not reflected in the world projection and the original link could not be restored.");
        }
        catch (Exception exception)
        {
            var restored = await RestoreLinkAsync(
                engine,
                world,
                instanceId,
                prefabFileId);
            return CommandResult.Failure(
                restored
                    ? $"Break prefab link failed: {exception.Message}. The original link was restored."
                    : $"Break prefab link failed: {exception.Message}. The original link could not be restored.");
        }
    }

    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        try
        {
            if (!await engine.SetPrefabLinkAsync(
                    instanceId,
                    prefabFileId,
                    CancellationToken.None))
            {
                return CommandResult.Failure("Restore prefab link failed");
            }

            if (IsProjectedLinked(
                    world,
                    instanceId,
                    prefabFileId))
            {
                return CommandResult.Success();
            }

            var restoredUnlinked = await RestoreUnlinkedAsync(
                engine,
                world,
                instanceId);
            return CommandResult.Failure(
                restoredUnlinked
                    ? "Restore prefab link was not reflected in the world projection; the unlinked state was restored."
                    : "Restore prefab link was not reflected in the world projection and the unlinked state could not be restored.");
        }
        catch (Exception exception)
        {
            var restoredUnlinked = await RestoreUnlinkedAsync(
                engine,
                world,
                instanceId);
            return CommandResult.Failure(
                restoredUnlinked
                    ? $"Restore prefab link failed: {exception.Message}. The unlinked state was restored."
                    : $"Restore prefab link failed: {exception.Message}. The unlinked state could not be restored.");
        }
    }

    static async Task<bool> RestoreLinkAsync(
        EngineService engine,
        WorldService world,
        InstanceId rootInstanceId,
        FileId sourcePrefabFileId)
    {
        try
        {
            await engine.SetPrefabLinkAsync(
                rootInstanceId,
                sourcePrefabFileId,
                CancellationToken.None);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Restore original prefab link cleanup failed: {exception}");
        }
        await RefreshProjectionSafelyAsync(
            engine,
            "Restore original prefab link");

        return IsProjectedLinked(
            world,
            rootInstanceId,
            sourcePrefabFileId);
    }

    static async Task<bool> RestoreUnlinkedAsync(
        EngineService engine,
        WorldService world,
        InstanceId rootInstanceId)
    {
        try
        {
            await engine.BreakPrefabLinkAsync(
                rootInstanceId,
                CancellationToken.None);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Restore unlinked prefab state cleanup failed: {exception}");
        }
        await RefreshProjectionSafelyAsync(
            engine,
            "Restore unlinked prefab state");

        return IsProjectedUnlinked(world, rootInstanceId);
    }

    static async Task RefreshProjectionSafelyAsync(
        EngineService engine,
        string operation)
    {
        try
        {
            await engine.RefreshCurrentWorldAsync(
                CancellationToken.None);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"{operation} projection refresh failed: {exception}");
        }
    }

    static bool IsProjectedLinked(
        WorldService world,
        InstanceId rootInstanceId,
        FileId sourcePrefabFileId)
    {
        if (!TryGetProjectedPrefab(
                world,
                rootInstanceId,
                out var prefab))
        {
            return false;
        }

        return prefab.FileId is not null &&
            prefab.FileId.Equals(sourcePrefabFileId);
    }

    static bool IsProjectedUnlinked(
        WorldService world,
        InstanceId rootInstanceId)
    {
        if (!TryGetProjectedPrefab(
                world,
                rootInstanceId,
                out var prefab))
        {
            return false;
        }

        return prefab.FileId is null || prefab.FileId.IsEmpty();
    }

    static bool TryGetProjectedPrefab(
        WorldService world,
        InstanceId rootInstanceId,
        out Prefab prefab)
    {
        prefab = null!;
        if (!world.TryGetGameObject(
                rootInstanceId,
                out var root) ||
            root.PrefabIndex < 0 ||
            root.PrefabIndex >= world.Current.Prefabs.Count)
        {
            return false;
        }

        prefab = world.Current.Prefabs[root.PrefabIndex];
        return true;
    }
}

public sealed class DestroyGameObjectCommand(GameObject gameObject) : IUndoableEditorCommand
{
    readonly CreatedHierarchySnapshot? _snapshot =
        CaptureSnapshot(gameObject);
    readonly InstanceId? _parentId =
        MauiProgram.GetService<WorldService>()
            .ResolveParentInstanceId(gameObject);
    readonly FileId? _prefabLinkFileId =
        ResolveLinkedRootPrefabFileId(gameObject);
    readonly InstanceId _activeInstanceId = gameObject.InstanceId;
    bool _deleteOutcomeAmbiguous;

    public string Name => nameof(DestroyGameObjectCommand);
    public string Description => $"Delete {gameObject.Name}";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) =>
        _snapshot is not null &&
        _activeInstanceId is not null &&
        !_activeInstanceId.IsEmpty();

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_snapshot is null)
        {
            return CommandResult.Failure(
                "Destroy failed: hierarchy snapshot was not captured");
        }

        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        var preflight =
            await AuthoritativeHierarchyReconciliation
                .RefreshAndClassifyAsync(
                    engine,
                    world,
                    _snapshot,
                    _parentId);
        if (preflight.Projection ==
            AuthoritativeHierarchyProjection.Unavailable)
        {
            return CommandResult.Failure(
                $"Destroy failed: authoritative preflight refresh failed{FormatDiagnostic(preflight.Diagnostic)}");
        }
        if (preflight.Projection ==
            AuthoritativeHierarchyProjection.Absent)
        {
            _deleteOutcomeAmbiguous = false;
            return RecoverySuccess(
                preflight.Projection,
                outcomeUncertain: false,
                "Hierarchy was already absent; a recovery history entry was retained.");
        }
        if (preflight.Projection !=
            AuthoritativeHierarchyProjection.Exact)
        {
            return CommandResult.Failure(
                "Destroy failed: the authoritative hierarchy no longer matches the saved snapshot");
        }

        var response = EngineMutationResponseState.Unknown;
        string? requestDiagnostic = null;
        try
        {
            response = await engine.RequestDestroyObjectAsync(
                    _activeInstanceId,
                    CancellationToken.None)
                ? EngineMutationResponseState.Accepted
                : EngineMutationResponseState.Rejected;
        }
        catch (Exception exception)
        {
            requestDiagnostic = exception.Message;
        }

        var postMutation =
            await AuthoritativeHierarchyReconciliation
                .RefreshAndClassifyAsync(
                    engine,
                    world,
                    _snapshot,
                    _parentId);
        var resolution = HierarchyMutationRecoveryPolicy.ResolveDestroy(
            response,
            postMutation.Projection);
        _deleteOutcomeAmbiguous = resolution.RetainRecoveryHistory;
        if (!resolution.Succeeded)
        {
            return CommandResult.Failure(
                $"Destroy was not applied{FormatDiagnostics(requestDiagnostic, postMutation.Diagnostic)}");
        }

        return RecoverySuccess(
            postMutation.Projection,
            resolution.OutcomeUncertain,
            resolution.OutcomeUncertain
                ? $"Destroy outcome is uncertain; the undo entry is retained for recovery{FormatDiagnostics(requestDiagnostic, postMutation.Diagnostic)}"
                : null);
    }

    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();

        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        if (_snapshot is null)
        {
            return CommandResult.Failure(
                "Restore deleted hierarchy failed: hierarchy snapshot was not captured");
        }

        var preflight =
            await AuthoritativeHierarchyReconciliation
                .RefreshAndClassifyAsync(
                    engine,
                    world,
                    _snapshot,
                    _parentId);
        if (preflight.Projection ==
            AuthoritativeHierarchyProjection.Unavailable)
        {
            return CommandResult.Failure(
                $"Restore deleted hierarchy failed: authoritative preflight refresh failed{FormatDiagnostic(preflight.Diagnostic)}");
        }

        var undoAction =
            HierarchyMutationRecoveryPolicy.ResolveDestroyUndo(
                _deleteOutcomeAmbiguous,
                preflight.Projection);
        if (undoAction ==
            DestroyHierarchyUndoAction.CompleteWithoutMutation)
        {
            _deleteOutcomeAmbiguous = false;
            return RecoverySuccess(
                preflight.Projection,
                outcomeUncertain: false,
                "Destroy was not applied; undo reconciled the original hierarchy without mutation.");
        }
        if (undoAction ==
            DestroyHierarchyUndoAction.FailWithoutMutation)
        {
            return CommandResult.Failure(
                "Restore deleted hierarchy failed: authoritative state does not match either the deleted or saved hierarchy; no mutation was attempted");
        }

        var result = await StrictHierarchyRestore.RestoreAsync(
            _snapshot,
            _parentId,
            _snapshot.DetachedFromPrefab ||
                _snapshot.LinkedPrefabSnapshot
                ? null
                : _prefabLinkFileId,
            "Restore deleted hierarchy failed");
        if (result.Succeeded)
        {
            _deleteOutcomeAmbiguous = false;
        }

        return result;
    }

    static FileId? ResolveLinkedRootPrefabFileId(GameObject gameObject)
    {
        if (gameObject.ParentIndex != uint.MaxValue ||
            !TryGetProjectedPrefab(gameObject, out var prefab))
        {
            return null;
        }

        var prefabFileId = prefab.FileId;
        return prefabFileId is null || prefabFileId.IsEmpty()
            ? null
            : new FileId(prefabFileId.Value);
    }

    static bool TryGetProjectedPrefab(
        GameObject gameObject,
        out Prefab prefab)
    {
        var world = MauiProgram.GetService<WorldService>();
        prefab = null!;
        if (gameObject.PrefabIndex < 0 ||
            gameObject.PrefabIndex >= world.Current.Prefabs.Count)
        {
            return false;
        }

        prefab = world.Current.Prefabs[gameObject.PrefabIndex];
        return true;
    }

    static CreatedHierarchySnapshot? CaptureSnapshot(
        GameObject root)
    {
        return CreatedHierarchySnapshot.TryCapture(
            MauiProgram.GetService<WorldService>(),
            root,
            out var snapshot,
            out _)
            ? snapshot
            : null;
    }

    CommandResult RecoverySuccess(
        AuthoritativeHierarchyProjection projection,
        bool outcomeUncertain,
        string? message)
        => CommandResult.Success(
            message,
            new HierarchyMutationRecoveryOutcome(
                "DestroyHierarchy",
                _activeInstanceId.Value,
                projection,
                outcomeUncertain));

    static string FormatDiagnostic(string? diagnostic) =>
        string.IsNullOrWhiteSpace(diagnostic)
            ? string.Empty
            : $": {diagnostic}";

    static string FormatDiagnostics(
        string? requestDiagnostic,
        string? refreshDiagnostic)
    {
        var diagnostics = new List<string>();
        if (!string.IsNullOrWhiteSpace(requestDiagnostic))
        {
            diagnostics.Add($"request: {requestDiagnostic}");
        }
        if (!string.IsNullOrWhiteSpace(refreshDiagnostic))
        {
            diagnostics.Add($"refresh: {refreshDiagnostic}");
        }

        return diagnostics.Count == 0
            ? string.Empty
            : $" ({string.Join("; ", diagnostics)})";
    }
}

public sealed class ReparentGameObjectCommand(GameObject child, GameObject? newParent, bool keepWorldTransform = true) : IUndoableEditorCommand
{
    readonly InstanceId _childId = child.InstanceId;
    readonly InstanceId? _newParentId = newParent?.InstanceId;
    readonly InstanceId? _oldParentId =
        MauiProgram.GetService<WorldService>()
            .ResolveParentInstanceId(child);
    public string Name => nameof(ReparentGameObjectCommand);
    public string Description => "Reparent GameObject";
    public IHistoryMergePolicy? MergePolicy => null;
    public bool CanExecute(ActionContext context) => _childId is not null && !_childId.IsEmpty();
    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        if (!await engine.ReparentObjectAsync(
                _childId,
                _newParentId,
                keepWorldTransform,
                cancellationToken))
            return CommandResult.Failure();

        return CommandResult.Success();
    }

    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        return await engine.ReparentObjectAsync(
            _childId,
            _oldParentId,
            keepWorldTransform,
            cancellationToken)
            ? CommandResult.Success()
            : CommandResult.Failure("Restore parent failed");
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
    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var world = MauiProgram.GetService<WorldService>();
        if (!world.TryGetGameObject(_ownerId, out var owner))
            return CommandResult.Failure("Owner not found");

        var engine = MauiProgram.GetService<EngineService>();
        var createdId = await engine.AddComponentAsync(
            _ownerId,
            componentTypeName,
            _componentId,
            cancellationToken);
        if (createdId is null)
            return CommandResult.Failure();

        _componentId = createdId;
        if (!world.TryGetComponent(createdId, out _))
        {
            await engine.RemoveComponentAsync(
                createdId,
                CancellationToken.None);
            return CommandResult.Failure("Created component was not projected");
        }

        return CommandResult.Success(value: _componentId);
    }
    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        if (_componentId is null || _componentId.IsEmpty())
            return CommandResult.Failure("Created component was not found");

        return await MauiProgram.GetService<EngineService>()
            .RemoveComponentAsync(_componentId, cancellationToken)
            ? CommandResult.Success()
            : CommandResult.Failure("Remove created component failed");
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

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
        => await MauiProgram.GetService<EngineService>()
            .RemoveComponentAsync(_activeInstanceId, cancellationToken)
            ? CommandResult.Success()
            : CommandResult.Failure();

    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();

        if (!world.TryGetGameObject(_ownerId, out var owner))
            return CommandResult.Failure("Component owner was not found");

        var restoredInstanceId = await engine.AddComponentAsync(
            _ownerId,
            _componentTypeName,
            _originalInstanceId,
            cancellationToken);
        if (restoredInstanceId is null)
            return CommandResult.Failure("Restore component failed");

        if (!restoredInstanceId.Equals(_originalInstanceId) || !world.TryGetComponent(restoredInstanceId, out var restored))
        {
            await engine.RemoveComponentAsync(
                restoredInstanceId,
                CancellationToken.None);
            return CommandResult.Failure("Restored component was not found");
        }

        if (!await engine.CommitChangesAsync(
                restored.InstanceId,
                _beforeYaml,
                cancellationToken))
        {
            await engine.RemoveComponentAsync(
                restored.InstanceId,
                CancellationToken.None);
            return CommandResult.Failure("Restore component state failed");
        }

        if (!world.ApplyComponentYamlLocal(restored.InstanceId, _beforeYaml))
        {
            await engine.RemoveComponentAsync(
                restored.InstanceId,
                CancellationToken.None);
            return CommandResult.Failure(
                "Refresh restored component state failed");
        }

        _activeInstanceId = restoredInstanceId;
        return CommandResult.Success();
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
    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var ok = await MauiProgram.GetService<EngineService>()
            .ResetComponentToDefaultsAsync(
                _instanceId,
                cancellationToken);
        if (ok && MauiProgram.GetService<WorldService>().TryGetComponent(_instanceId, out var refreshed))
            _afterYaml = EditorYaml.SerializeComponent(refreshed);
        return ok ? CommandResult.Success() : CommandResult.Failure();
    }
    public async ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        if (!await MauiProgram.GetService<EngineService>()
                .CommitChangesAsync(
                    _instanceId,
                    _beforeYaml,
                    cancellationToken))
            return CommandResult.Failure("Restore component state failed");

        return MauiProgram.GetService<WorldService>()
            .ApplyComponentYamlLocal(_instanceId, _beforeYaml)
            ? CommandResult.Success()
            : CommandResult.Failure("Refresh restored component state failed");
    }
}
