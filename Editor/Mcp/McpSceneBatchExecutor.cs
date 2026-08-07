#nullable enable

using SailorEditor.AI;
using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using ViewModelComponent = SailorEditor.ViewModels.Component;

namespace SailorEditor.Mcp;

internal sealed class McpSceneBatchExecutor
{
    readonly ICommandDispatcher _dispatcher;
    readonly ITransactionScopeFactory _transactions;
    readonly IActionContextProvider _contextProvider;
    readonly ICommandHistoryService _history;
    readonly WorldService _world;
    readonly EngineService _engine;
    readonly AssetsService _assets;
    readonly AIOperatorService _aiOperator;

    public McpSceneBatchExecutor(
        ICommandDispatcher dispatcher,
        ITransactionScopeFactory transactions,
        IActionContextProvider contextProvider,
        ICommandHistoryService history,
        WorldService world,
        EngineService engine,
        AssetsService assets,
        AIOperatorService aiOperator)
    {
        _dispatcher = dispatcher;
        _transactions = transactions;
        _contextProvider = contextProvider;
        _history = history;
        _world = world;
        _engine = engine;
        _assets = assets;
        _aiOperator = aiOperator;
    }

    public async Task<McpSceneBatchResult> ExecuteAsync(
        McpSceneBatchRequest request,
        CancellationToken cancellationToken = default)
    {
        if (!request.Confirm)
        {
            return new McpSceneBatchResult(
                false,
                "Scene mutations require confirm=true.",
                _history.WorkspaceEpoch,
                Array.Empty<McpSceneOperationResult>());
        }
        if (request.ExpectedWorkspaceEpoch is { } expectedEpoch &&
            expectedEpoch != _history.WorkspaceEpoch)
        {
            return new McpSceneBatchResult(
                false,
                $"Stale workspace epoch {expectedEpoch}; current epoch is {_history.WorkspaceEpoch}. " +
                    "Refresh editor state before retrying the batch.",
                _history.WorkspaceEpoch,
                Array.Empty<McpSceneOperationResult>());
        }
        if (request.Operations is null || request.Operations.Count == 0)
        {
            return new McpSceneBatchResult(
                false,
                "At least one scene operation is required.",
                _history.WorkspaceEpoch,
                Array.Empty<McpSceneOperationResult>());
        }

        var description = string.IsNullOrWhiteSpace(request.Description)
            ? $"MCP scene batch ({request.Operations.Count} operations)"
            : request.Description.Trim();
        var metadata = new Dictionary<string, string?>
        {
            ["mcp"] = "true",
            ["mcpOperationCount"] = request.Operations.Count.ToString(),
            ["mcpDescription"] = description,
        };
        var context = _contextProvider.GetCurrentContext(
            new CommandOrigin(CommandOriginKind.AI, "MCP", "External MCP Agent"),
            metadata);
        var aliases = new Dictionary<string, InstanceId>(StringComparer.Ordinal);
        var results = new List<McpSceneOperationResult>(request.Operations.Count);
        var auditItems = new List<AIActionExecutionItem>(request.Operations.Count);

        try
        {
            await using var transaction = await _transactions.BeginAsync(
                description,
                context,
                cancellationToken);
            for (var index = 0; index < request.Operations.Count; index++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var operation = request.Operations[index];
                var result = await ExecuteOperationAsync(
                    operation,
                    aliases,
                    context,
                    cancellationToken);
                results.Add(new McpSceneOperationResult(
                    index,
                    operation.Kind,
                    result.Succeeded,
                    (result.Value as InstanceId)?.Value,
                    operation.Alias,
                    result.Message));
                auditItems.Add(new AIActionExecutionItem(
                    operation.Kind,
                    result.Succeeded,
                    result.Message));
                if (!result.Succeeded)
                {
                    var message = result.Message ??
                        $"Scene operation {index} ('{operation.Kind}') failed.";
                    _aiOperator.RecordExternalExecution(
                        description,
                        AIActionSafety.ConfirmRequired,
                        AIProposalState.Failed,
                        auditItems,
                        message);
                    return new McpSceneBatchResult(
                        false,
                        message + " The batch was rolled back.",
                        _history.WorkspaceEpoch,
                        results);
                }

                if (!string.IsNullOrWhiteSpace(operation.Alias) &&
                    result.Value is InstanceId instanceId)
                {
                    if (!aliases.TryAdd(operation.Alias, instanceId))
                    {
                        var message = $"Duplicate operation alias '{operation.Alias}'.";
                        auditItems.Add(new AIActionExecutionItem(
                            operation.Kind,
                            false,
                            message));
                        _aiOperator.RecordExternalExecution(
                            description,
                            AIActionSafety.ConfirmRequired,
                            AIProposalState.Failed,
                            auditItems,
                            message);
                        return new McpSceneBatchResult(
                            false,
                            message + " The batch was rolled back.",
                            _history.WorkspaceEpoch,
                            results);
                    }
                }
            }

            await transaction.CompleteAsync(cancellationToken);
            _aiOperator.RecordExternalExecution(
                description,
                AIActionSafety.ConfirmRequired,
                AIProposalState.Executed,
                auditItems,
                description);
            return new McpSceneBatchResult(
                true,
                description,
                _history.WorkspaceEpoch,
                results);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception exception)
        {
            _aiOperator.RecordExternalExecution(
                description,
                AIActionSafety.ConfirmRequired,
                AIProposalState.Failed,
                auditItems,
                exception.Message);
            return new McpSceneBatchResult(
                false,
                exception.Message + " The batch was rolled back.",
                _history.WorkspaceEpoch,
                results);
        }
    }

    async Task<CommandResult> ExecuteOperationAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        var kind = operation.Kind?.Trim().ToLowerInvariant();
        return kind switch
        {
            "create_game_object" => await CreateGameObjectAsync(
                operation,
                aliases,
                context,
                cancellationToken),
            "update_game_object" => await UpdateGameObjectAsync(
                operation,
                aliases,
                context,
                cancellationToken),
            "destroy_game_object" => await DispatchForGameObjectAsync(
                operation,
                aliases,
                context,
                gameObject => new DestroyGameObjectCommand(gameObject),
                cancellationToken),
            "reparent_game_object" => await ReparentGameObjectAsync(
                operation,
                aliases,
                context,
                cancellationToken),
            "add_component" => await AddComponentAsync(
                operation,
                aliases,
                context,
                cancellationToken),
            "update_component" => await UpdateComponentAsync(
                operation,
                aliases,
                context,
                cancellationToken),
            "remove_component" => await DispatchForComponentAsync(
                operation,
                aliases,
                context,
                component => new RemoveComponentCommand(component),
                cancellationToken),
            "reset_component" => await DispatchForComponentAsync(
                operation,
                aliases,
                context,
                component => new ResetComponentToDefaultsCommand(component),
                cancellationToken),
            "select" => await SelectAsync(
                operation,
                aliases,
                context,
                cancellationToken),
            "focus" => await FocusAsync(
                operation,
                aliases,
                context,
                cancellationToken),
            _ => CommandResult.Failure(
                $"Unknown scene operation '{operation.Kind}'."),
        };
    }

    async Task<CommandResult> CreateGameObjectAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        var parentId = ResolveOptionalReference(operation.Parent, aliases);
        if (!string.IsNullOrWhiteSpace(operation.Parent) && parentId is null)
            return CommandResult.Failure($"Parent '{operation.Parent}' was not found.");

        var created = await _dispatcher.DispatchAsync(
            new CreateGameObjectCommand(parentId),
            context,
            cancellationToken);
        if (!created.Succeeded || created.Value is not InstanceId createdId)
            return created.Succeeded
                ? CommandResult.Failure("CreateGameObject did not return an instance id.")
                : created;

        if (!_world.TryGetGameObject(createdId, out var gameObject))
            return CommandResult.Failure("Created GameObject was not projected.");

        var properties = new Dictionary<string, System.Text.Json.JsonElement>(
            operation.Properties ?? new Dictionary<string, System.Text.Json.JsonElement>(),
            StringComparer.Ordinal);
        if (!string.IsNullOrWhiteSpace(operation.Name))
        {
            properties["name"] = System.Text.Json.JsonSerializer.SerializeToElement(
                operation.Name.Trim());
        }

        if (properties.Count > 0)
        {
            var update = McpSceneCommandFactory.CreateGameObjectUpdate(
                gameObject,
                properties,
                $"Configure {operation.Name ?? "GameObject"}");
            var updated = await _dispatcher.DispatchAsync(
                update,
                context,
                cancellationToken);
            if (!updated.Succeeded)
                return updated;
        }

        return CommandResult.Success(value: createdId);
    }

    async Task<CommandResult> UpdateGameObjectAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        if (!TryResolveGameObject(operation.Target, aliases, out var gameObject, out var error))
            return CommandResult.Failure(error);

        var properties = new Dictionary<string, System.Text.Json.JsonElement>(
            operation.Properties ?? new Dictionary<string, System.Text.Json.JsonElement>(),
            StringComparer.Ordinal);
        if (!string.IsNullOrWhiteSpace(operation.Name))
            properties["name"] = System.Text.Json.JsonSerializer.SerializeToElement(operation.Name.Trim());
        if (properties.Count == 0)
            return CommandResult.Failure("No GameObject properties were provided.");

        var command = McpSceneCommandFactory.CreateGameObjectUpdate(
            gameObject,
            properties,
            $"MCP edit {gameObject.Name}");
        var result = await _dispatcher.DispatchAsync(command, context, cancellationToken);
        return result.Succeeded
            ? result with { Value = gameObject.InstanceId }
            : result;
    }

    async Task<CommandResult> ReparentGameObjectAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        if (!TryResolveGameObject(operation.Target, aliases, out var child, out var error))
            return CommandResult.Failure(error);

        GameObject? parent = null;
        if (!string.IsNullOrWhiteSpace(operation.Parent))
        {
            var parentId = ResolveOptionalReference(operation.Parent, aliases);
            if (parentId is null || !_world.TryGetGameObject(parentId, out parent))
                return CommandResult.Failure($"Parent '{operation.Parent}' was not found.");
        }

        var result = await _dispatcher.DispatchAsync(
            new ReparentGameObjectCommand(
                child,
                parent,
                operation.KeepWorldTransform),
            context,
            cancellationToken);
        return result.Succeeded
            ? result with { Value = child.InstanceId }
            : result;
    }

    async Task<CommandResult> AddComponentAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        if (!TryResolveGameObject(operation.Target, aliases, out var owner, out var error))
            return CommandResult.Failure(error);
        if (string.IsNullOrWhiteSpace(operation.ComponentType) ||
            !_engine.EngineTypes.IsAddableComponentType(operation.ComponentType) ||
            !_engine.EngineTypes.TryGetComponent(operation.ComponentType, out var componentType))
        {
            return CommandResult.Failure(
                $"Component type '{operation.ComponentType}' is unknown or cannot be added.");
        }

        if (operation.Properties?.Count > 0)
        {
            var validationError = await ValidateComponentPropertiesAsync(
                componentType,
                operation.Properties,
                cancellationToken);
            if (validationError is not null)
                return CommandResult.Failure(validationError);
        }

        var added = await _dispatcher.DispatchAsync(
            new AddComponentCommand(owner, operation.ComponentType),
            context,
            cancellationToken);
        if (!added.Succeeded || added.Value is not InstanceId componentId)
            return added.Succeeded
                ? CommandResult.Failure("AddComponent did not return an instance id.")
                : added;
        if (!_world.TryGetComponent(componentId, out var component))
            return CommandResult.Failure("Created component was not projected.");

        if (operation.Properties?.Count > 0)
        {
            var update = McpSceneCommandFactory.CreateComponentUpdate(
                component,
                operation.Properties,
                $"Configure {operation.ComponentType}");
            var updated = await _dispatcher.DispatchAsync(
                update,
                context,
                cancellationToken);
            if (!updated.Succeeded)
                return updated;
        }

        return CommandResult.Success(value: componentId);
    }

    async Task<CommandResult> UpdateComponentAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        if (!TryResolveComponent(operation.Target, aliases, out var component, out var error))
            return CommandResult.Failure(error);
        if (operation.Properties is null || operation.Properties.Count == 0)
            return CommandResult.Failure("No component properties were provided.");

        var validationError = await ValidateComponentPropertiesAsync(
            component.Typename,
            operation.Properties,
            cancellationToken);
        if (validationError is not null)
            return CommandResult.Failure(validationError);

        var command = McpSceneCommandFactory.CreateComponentUpdate(
            component,
            operation.Properties,
            $"MCP edit {component.Typename?.Name}");
        var result = await _dispatcher.DispatchAsync(command, context, cancellationToken);
        return result.Succeeded
            ? result with { Value = component.InstanceId }
            : result;
    }

    async Task<CommandResult> DispatchForGameObjectAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        Func<GameObject, IEditorCommand> createCommand,
        CancellationToken cancellationToken)
    {
        if (!TryResolveGameObject(operation.Target, aliases, out var gameObject, out var error))
            return CommandResult.Failure(error);
        var instanceId = gameObject.InstanceId;
        var result = await _dispatcher.DispatchAsync(
            createCommand(gameObject),
            context,
            cancellationToken);
        return result.Succeeded
            ? result with { Value = instanceId }
            : result;
    }

    async Task<CommandResult> DispatchForComponentAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        Func<ViewModelComponent, IEditorCommand> createCommand,
        CancellationToken cancellationToken)
    {
        if (!TryResolveComponent(operation.Target, aliases, out var component, out var error))
            return CommandResult.Failure(error);
        var instanceId = component.InstanceId;
        var result = await _dispatcher.DispatchAsync(
            createCommand(component),
            context,
            cancellationToken);
        return result.Succeeded
            ? result with { Value = instanceId }
            : result;
    }

    async Task<CommandResult> SelectAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        var instanceId = ResolveOptionalReference(operation.Target, aliases);
        if (!string.IsNullOrWhiteSpace(operation.Target) && instanceId is null)
            return CommandResult.Failure($"Target '{operation.Target}' was not found.");
        var result = await _dispatcher.DispatchAsync(
            new SelectObjectCommand(instanceId),
            context,
            cancellationToken);
        return result.Succeeded
            ? result with { Value = instanceId }
            : result;
    }

    async Task<CommandResult> FocusAsync(
        McpSceneOperation operation,
        IReadOnlyDictionary<string, InstanceId> aliases,
        ActionContext context,
        CancellationToken cancellationToken)
    {
        var instanceId = ResolveOptionalReference(operation.Target, aliases);
        if (instanceId is null)
            return CommandResult.Failure($"Target '{operation.Target}' was not found.");
        var result = await _dispatcher.DispatchAsync(
            new FocusEditorCameraCommand(instanceId),
            context,
            cancellationToken);
        return result.Succeeded
            ? result with { Value = instanceId }
            : result;
    }

    bool TryResolveGameObject(
        string? reference,
        IReadOnlyDictionary<string, InstanceId> aliases,
        out GameObject gameObject,
        out string error)
    {
        var instanceId = ResolveOptionalReference(reference, aliases);
        if (instanceId is not null && _world.TryGetGameObject(instanceId, out gameObject!))
        {
            error = string.Empty;
            return true;
        }

        gameObject = null!;
        error = $"GameObject '{reference}' was not found.";
        return false;
    }

    bool TryResolveComponent(
        string? reference,
        IReadOnlyDictionary<string, InstanceId> aliases,
        out ViewModelComponent component,
        out string error)
    {
        var instanceId = ResolveOptionalReference(reference, aliases);
        if (instanceId is not null && _world.TryGetComponent(instanceId, out component!))
        {
            error = string.Empty;
            return true;
        }

        component = null!;
        error = $"Component '{reference}' was not found.";
        return false;
    }

    static InstanceId? ResolveOptionalReference(
        string? reference,
        IReadOnlyDictionary<string, InstanceId> aliases)
    {
        if (string.IsNullOrWhiteSpace(reference))
            return null;
        var normalized = reference.Trim();
        if (normalized.StartsWith('$'))
            return aliases.TryGetValue(normalized[1..], out var aliasValue)
                ? aliasValue
                : null;

        return new InstanceId(normalized);
    }

    async Task<string?> ValidateComponentPropertiesAsync(
        ComponentType? componentType,
        IReadOnlyDictionary<string, System.Text.Json.JsonElement> properties,
        CancellationToken cancellationToken)
    {
        if (componentType is null)
            return "The component has no reflected type.";

        foreach (var property in properties)
        {
            if (string.Equals(property.Key, "instanceId", StringComparison.Ordinal) ||
                string.Equals(property.Key, "fileId", StringComparison.Ordinal) ||
                !componentType.Properties.TryGetValue(property.Key, out var descriptor))
            {
                return $"Unknown or immutable property '{property.Key}' for component " +
                    $"'{componentType.Name}'.";
            }
            if (componentType.ReadOnlyProperties.Contains(property.Key))
            {
                return $"Property '{componentType.Name}.{property.Key}' is read-only.";
            }
            if (descriptor is not ObjectPtrProperty objectPointer)
            {
                continue;
            }

            if (property.Value.ValueKind != System.Text.Json.JsonValueKind.Object)
            {
                return $"Property '{componentType.Name}.{property.Key}' requires an object " +
                    "with fileId and/or instanceId.";
            }

            var hasFileId = property.Value.TryGetProperty("fileId", out var fileIdNode);
            var hasInstanceId = property.Value.TryGetProperty("instanceId", out var instanceIdNode);
            if (!hasFileId && !hasInstanceId)
            {
                return $"Property '{componentType.Name}.{property.Key}' requires fileId " +
                    "and/or instanceId.";
            }
            foreach (var referenceProperty in property.Value.EnumerateObject())
            {
                if (referenceProperty.Name is not ("fileId" or "instanceId"))
                {
                    return $"Property '{componentType.Name}.{property.Key}' only accepts " +
                        "fileId and instanceId.";
                }
            }
            if (hasFileId && fileIdNode.ValueKind != System.Text.Json.JsonValueKind.String)
            {
                return $"Property '{componentType.Name}.{property.Key}.fileId' must be a string.";
            }
            if (hasInstanceId &&
                instanceIdNode.ValueKind != System.Text.Json.JsonValueKind.String)
            {
                return $"Property '{componentType.Name}.{property.Key}.instanceId' must be a string.";
            }

            if (hasFileId)
            {
                var fileIdValue = fileIdNode.GetString();
                if (!string.IsNullOrWhiteSpace(fileIdValue) &&
                    fileIdValue != FileId.NullFileId)
                {
                    var asset = await _assets.ResolveAssetAsync(
                        new FileId(fileIdValue),
                        cancellationToken);
                    if (asset is null)
                    {
                        return $"Asset '{fileIdValue}' referenced by " +
                            $"'{componentType.Name}.{property.Key}' was not found.";
                    }
                    if (objectPointer.GenericType is not null &&
                        !objectPointer.GenericType.IsInstanceOfType(asset))
                    {
                        return $"Asset '{fileIdValue}' has type '{asset.GetType().Name}', but " +
                            $"'{componentType.Name}.{property.Key}' requires " +
                            $"'{objectPointer.GenericTypename}'.";
                    }
                }
            }

            if (hasInstanceId)
            {
                var instanceIdValue = instanceIdNode.GetString();
                if (!string.IsNullOrWhiteSpace(instanceIdValue) &&
                    instanceIdValue != InstanceId.NullInstanceId &&
                    !IsCompatibleInstanceReference(
                        new InstanceId(instanceIdValue),
                        objectPointer.GenericTypename))
                {
                    return $"Instance '{instanceIdValue}' is not compatible with " +
                        $"'{componentType.Name}.{property.Key}' " +
                        $"({objectPointer.GenericTypename}).";
                }
            }
        }

        return null;
    }

    bool IsCompatibleInstanceReference(
        InstanceId instanceId,
        string expectedTypeName)
    {
        if (_world.TryGetGameObject(instanceId, out _))
        {
            return string.IsNullOrWhiteSpace(expectedTypeName) ||
                expectedTypeName.EndsWith("GameObject", StringComparison.Ordinal);
        }
        if (!_world.TryGetComponent(instanceId, out var component))
            return false;
        if (string.IsNullOrWhiteSpace(expectedTypeName) ||
            expectedTypeName.EndsWith("::Component", StringComparison.Ordinal))
        {
            return true;
        }

        var actualTypeName = component.Typename?.Name;
        return string.Equals(expectedTypeName, actualTypeName, StringComparison.Ordinal) ||
            expectedTypeName.EndsWith(
                "::" + actualTypeName?.Split("::").LastOrDefault(),
                StringComparison.Ordinal);
    }
}
