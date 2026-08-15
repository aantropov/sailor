#nullable enable

using ModelContextProtocol.Server;
using SailorEditor.AI;
using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.Workspace;

namespace SailorEditor.Mcp;

public sealed record McpEditorStateSnapshot(
    string ProjectMode,
    string ProjectName,
    string ProjectRoot,
    string? WorldId,
    string EngineState,
    bool IsPlayMode,
    IReadOnlyList<string> SelectionIds,
    long WorkspaceEpoch,
    int UndoCount,
    int RedoCount);

internal sealed class McpEditorTools
{
    readonly IEditorThreadDispatcher _editorThread;
    readonly IAIEditorContextProvider _contextProvider;
    readonly WorkspaceUiService _workspaceUi;
    readonly EngineService _engine;
    readonly ICommandHistoryService _history;
    readonly AIOperatorService _aiOperator;
    readonly McpSceneSnapshotBuilder _sceneSnapshots;
    readonly McpSceneBatchExecutor _sceneBatch;
    readonly McpLandscapeOperations _landscapeOperations;
    readonly McpAssetOperations _assetOperations;
    readonly McpWorkspaceOperations _workspaceOperations;
    readonly McpCSharpEvaluator _csharpEvaluator;

    public McpEditorTools(
        IEditorThreadDispatcher editorThread,
        IAIEditorContextProvider contextProvider,
        WorkspaceUiService workspaceUi,
        EngineService engine,
        ICommandHistoryService history,
        AIOperatorService aiOperator,
        McpSceneSnapshotBuilder sceneSnapshots,
        McpSceneBatchExecutor sceneBatch,
        McpLandscapeOperations landscapeOperations,
        McpAssetOperations assetOperations,
        McpWorkspaceOperations workspaceOperations,
        McpCSharpEvaluator csharpEvaluator)
    {
        _editorThread = editorThread;
        _contextProvider = contextProvider;
        _workspaceUi = workspaceUi;
        _engine = engine;
        _history = history;
        _aiOperator = aiOperator;
        _sceneSnapshots = sceneSnapshots;
        _sceneBatch = sceneBatch;
        _landscapeOperations = landscapeOperations;
        _assetOperations = assetOperations;
        _workspaceOperations = workspaceOperations;
        _csharpEvaluator = csharpEvaluator;
    }

    public McpServerTool[] CreateTools() =>
    [
        McpServerTool.Create(
            (CancellationToken cancellationToken) =>
                GetEditorStateAsync(cancellationToken),
            new()
            {
                Name = "sailor_editor_get_state",
                Description =
                    "Get the active Sailor Editor project, engine, world, selection and undo state.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool includeYaml, CancellationToken cancellationToken) =>
                GetSceneHierarchyAsync(includeYaml, cancellationToken),
            new()
            {
                Name = "sailor_scene_get_hierarchy",
                Description =
                    "Get the current scene hierarchy with GameObject/component instance IDs. " +
                    "Set includeYaml=true only when the full native scene document is needed.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (string instanceId, CancellationToken cancellationToken) =>
                GetSceneObjectAsync(instanceId, cancellationToken),
            new()
            {
                Name = "sailor_scene_get_object",
                Description = "Get one current GameObject and its components by instance ID.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (CancellationToken cancellationToken) =>
                GetComponentTypesAsync(cancellationToken),
            new()
            {
                Name = "sailor_types_list_components",
                Description =
                    "List canonical reflected component types, editable properties, defaults, enums and object-reference target types.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm,
                long? expectedWorkspaceEpoch,
                string? description,
                McpSceneOperation[] operations,
                CancellationToken cancellationToken) =>
                ApplySceneBatchAsync(
                    confirm,
                    expectedWorkspaceEpoch,
                    description,
                    operations,
                    cancellationToken),
            new()
            {
                Name = "sailor_scene_apply_batch",
                Description =
                    "Atomically mutate the current scene through normal Editor commands. " +
                    "Supported kinds: create_game_object, update_game_object, destroy_game_object, " +
                    "reparent_game_object, add_component, update_component, remove_component, " +
                    "reset_component, select, focus. References beginning with '$' resolve an earlier operation alias. " +
                    "Pass the workspace epoch returned by sailor_editor_get_state to reject stale plans. " +
                    "Mutations require confirm=true and successful batches are one undo entry.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (McpLandscapeApplyRequest request, CancellationToken cancellationToken) =>
                ApplyLandscapeAsync(request, cancellationToken),
            new()
            {
                Name = "sailor_landscape_apply",
                Description =
                    "Create or fully author a LandscapeComponent from a level description. " +
                    "Resolve material, texture and model fileIds with sailor_assets_list/get first. " +
                    "The request replaces terrain stamps and all vegetation profiles atomically. " +
                    "A vegetation materialFileId is optional; leave it empty to use the GLB materials. " +
                    "Vegetation shadows accept None, NearOnly, or All. Leave targetComponentId empty to create a new Landscape GameObject. " +
                    "Pass the current workspace epoch and confirm=true; the complete edit is one undo entry.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm,
                long? expectedWorkspaceEpoch,
                string targetComponentId,
                CancellationToken cancellationToken) =>
                RegenerateLandscapeAsync(
                    confirm,
                    expectedWorkspaceEpoch,
                    targetComponentId,
                    cancellationToken),
            new()
            {
                Name = "sailor_landscape_regenerate",
                Description =
                    "Regenerate terrain, vegetation, render proxies and collision for an existing LandscapeComponent. " +
                    "Requires its component instance ID, the current workspace epoch and confirm=true.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm,
                string code,
                int timeoutMs,
                CancellationToken cancellationToken) =>
                EvalCSharpAsync(confirm, code, timeoutMs, cancellationToken),
            new()
            {
                Name = "sailor_editor_eval_csharp",
                Description =
                    "Compile trusted C# code out of process, then evaluate it inside Sailor Editor with Services, EditorThread, " +
                    "CancellationToken and Print available. The code is the body of an async method; use return to provide a result. " +
                    "This is an unrestricted local operation and requires confirm=true. timeoutMs is clamped to 100..30000.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm, CancellationToken cancellationToken) =>
                SaveSceneAsync(confirm, cancellationToken),
            new()
            {
                Name = "sailor_scene_save",
                Description =
                    "Save the current scene through the normal Editor save command. " +
                    "An untitled scene opens the standard save dialog. Requires confirm=true.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm,
                string configuration,
                bool configure,
                CancellationToken cancellationToken) =>
                BuildWorkspaceAsync(
                    confirm,
                    configuration,
                    configure,
                    cancellationToken),
            new()
            {
                Name = "sailor_workspace_build",
                Description =
                    "Configure and build the active workspace generated CMake project. " +
                    "Configuration may be Debug, Release, RelWithDebInfo or MinSizeRel. Requires confirm=true.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm, CancellationToken cancellationToken) =>
                UndoAsync(confirm, cancellationToken),
            new()
            {
                Name = "sailor_scene_undo",
                Description = "Undo the latest Editor history entry. Requires confirm=true.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm, CancellationToken cancellationToken) =>
                RedoAsync(confirm, cancellationToken),
            new()
            {
                Name = "sailor_scene_redo",
                Description = "Redo the latest Editor history entry. Requires confirm=true.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (string? path,
                string? type,
                bool recursive,
                CancellationToken cancellationToken) =>
                ListAssetsAsync(path, type, recursive, cancellationToken),
            new()
            {
                Name = "sailor_assets_list",
                Description =
                    "List assets known to the Editor cache under a path. " +
                    "Relative paths are searched in every mounted Content root; an empty path searches all roots.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (string target, CancellationToken cancellationToken) =>
                GetAssetAsync(target, cancellationToken),
            new()
            {
                Name = "sailor_assets_get",
                Description =
                    "Resolve an asset by fileId or source path, lazily load its typed asset info and return its properties.",
                UseStructuredContent = true,
            }),
        McpServerTool.Create(
            (bool confirm,
                string kind,
                string? target,
                string? destinationFolder,
                string? newName,
                string? folderPath,
                string? folderName,
                CancellationToken cancellationToken) =>
                ApplyAssetOperationAsync(
                    confirm,
                    kind,
                    target,
                    destinationFolder,
                    newName,
                    folderPath,
                    folderName,
                    cancellationToken),
            new()
            {
                Name = "sailor_assets_apply",
                Description =
                    "Run one normal Editor asset command. Supported kinds: rename_asset, delete_asset, " +
                    "duplicate_asset, move_asset, reimport_asset, create_folder, rename_folder, " +
                    "delete_folder, move_folder. Mutations require confirm=true.",
                UseStructuredContent = true,
            }),
    ];

    public Task<McpEditorStateSnapshot> GetEditorStateAsync(
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () =>
            {
                var projection = _workspaceUi.Projection;
                var context = _contextProvider.GetCurrentContext();
                return Task.FromResult(new McpEditorStateSnapshot(
                    projection.Mode.ToString(),
                    projection.ActiveProjectName,
                    projection.ActiveRootPath,
                    context.ActiveWorldId,
                    _engine.State.ToString(),
                    context.IsPlayMode,
                    context.SelectionIds,
                    _history.WorkspaceEpoch,
                    _history.UndoCount,
                    _history.RedoCount));
            },
            cancellationToken);

    public Task<McpSceneHierarchySnapshot> GetSceneHierarchyAsync(
        bool includeYaml,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => Task.FromResult(_sceneSnapshots.Build(includeYaml)),
            cancellationToken);

    public Task<McpGameObjectSnapshot?> GetSceneObjectAsync(
        string instanceId,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => Task.FromResult(_sceneSnapshots.BuildObject(instanceId)),
            cancellationToken);

    public Task<IReadOnlyList<McpComponentTypeSchema>> GetComponentTypesAsync(
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => Task.FromResult(_sceneSnapshots.BuildComponentSchemas()),
            cancellationToken);

    public Task<McpSceneBatchResult> ApplySceneBatchAsync(
        bool confirm,
        long? expectedWorkspaceEpoch,
        string? description,
        IReadOnlyList<McpSceneOperation> operations,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => _sceneBatch.ExecuteAsync(
                new McpSceneBatchRequest(
                    confirm,
                    expectedWorkspaceEpoch,
                    description,
                    operations ?? Array.Empty<McpSceneOperation>()),
                cancellationToken),
            cancellationToken);

    public Task<McpSceneBatchResult> ApplyLandscapeAsync(
        McpLandscapeApplyRequest request,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => _landscapeOperations.ApplyAsync(request, cancellationToken),
            cancellationToken);

    public Task<McpSceneBatchResult> RegenerateLandscapeAsync(
        bool confirm,
        long? expectedWorkspaceEpoch,
        string targetComponentId,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => _landscapeOperations.RegenerateAsync(
                confirm,
                expectedWorkspaceEpoch,
                targetComponentId,
                cancellationToken),
            cancellationToken);

    public Task<object> UndoAsync(
        bool confirm,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync<object>(
            async () =>
            {
                if (!confirm)
                    return new { succeeded = false, message = "Undo requires confirm=true.", undoCount = _history.UndoCount, redoCount = _history.RedoCount };
                var succeeded = await _history.UndoAsync(
                    new CommandOrigin(CommandOriginKind.AI, "MCP", "External MCP Agent"),
                    cancellationToken);
                RecordHistoryExecution("MCP undo", succeeded);
                return new { succeeded, message = succeeded ? "Undo completed." : "Nothing was undone.", undoCount = _history.UndoCount, redoCount = _history.RedoCount };
            },
            cancellationToken);

    public async Task<McpCSharpEvalResult> EvalCSharpAsync(
        bool confirm,
        string code,
        int timeoutMs,
        CancellationToken cancellationToken = default)
    {
        if (!confirm)
            return new McpCSharpEvalResult(false, null, null, string.Empty, "C# evaluation requires confirm=true.");

        var result = await Task.Run(
            () => _csharpEvaluator.EvaluateAsync(code, timeoutMs, cancellationToken),
            cancellationToken);
        await _editorThread.InvokeAsync(
            () =>
            {
                _aiOperator.RecordExternalExecution(
                    "MCP C# eval",
                    AIActionSafety.ConfirmRequired,
                    result.Succeeded ? AIProposalState.Executed : AIProposalState.Failed,
                    [new AIActionExecutionItem("Evaluate C#", result.Succeeded, result.Error)],
                    result.Succeeded ? "C# evaluation completed." : "C# evaluation failed.");
                return Task.CompletedTask;
            },
            cancellationToken);
        return result;
    }

    public Task<object> RedoAsync(
        bool confirm,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync<object>(
            async () =>
            {
                if (!confirm)
                    return new { succeeded = false, message = "Redo requires confirm=true.", undoCount = _history.UndoCount, redoCount = _history.RedoCount };
                var succeeded = await _history.RedoAsync(
                    new CommandOrigin(CommandOriginKind.AI, "MCP", "External MCP Agent"),
                    cancellationToken);
                RecordHistoryExecution("MCP redo", succeeded);
                return new { succeeded, message = succeeded ? "Redo completed." : "Nothing was redone.", undoCount = _history.UndoCount, redoCount = _history.RedoCount };
            },
            cancellationToken);

    void RecordHistoryExecution(string title, bool succeeded) =>
        _aiOperator.RecordExternalExecution(
            title,
            AIActionSafety.ConfirmRequired,
            succeeded ? AIProposalState.Executed : AIProposalState.Failed,
            [new AIActionExecutionItem(title, succeeded, null)],
            succeeded ? title + " completed." : title + " did not change history.");

    public Task<IReadOnlyList<McpAssetSnapshot>> ListAssetsAsync(
        string? path,
        string? type,
        bool recursive,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => _assetOperations.ListAsync(
                path,
                type,
                recursive,
                cancellationToken),
            cancellationToken);

    public Task<McpAssetSnapshot?> GetAssetAsync(
        string target,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => _assetOperations.GetAsync(target, cancellationToken),
            cancellationToken);

    public Task<McpAssetMutationResult> ApplyAssetOperationAsync(
        bool confirm,
        string kind,
        string? target,
        string? destinationFolder,
        string? newName,
        string? folderPath,
        string? folderName,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync(
            () => _assetOperations.ExecuteAsync(
                confirm,
                kind,
                target,
                destinationFolder,
                newName,
                folderPath,
                folderName,
                cancellationToken),
            cancellationToken);

    public Task<object> SaveSceneAsync(
        bool confirm,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync<object>(
            async () =>
            {
                var result = await _workspaceOperations.SaveSceneAsync(
                    confirm,
                    cancellationToken);
                return new
                {
                    result.Succeeded,
                    result.Message,
                    result.Value,
                };
            },
            cancellationToken);

    public Task<object> BuildWorkspaceAsync(
        bool confirm,
        string configuration,
        bool configure,
        CancellationToken cancellationToken = default) =>
        _editorThread.InvokeAsync<object>(
            async () =>
            {
                var result = await _workspaceOperations.BuildWorkspaceAsync(
                    confirm,
                    configuration,
                    configure,
                    cancellationToken);
                if (result.Value is WorkspaceBuildResult buildResult)
                    return buildResult;

                return new
                {
                    result.Succeeded,
                    result.Message,
                };
            },
            cancellationToken);
}
