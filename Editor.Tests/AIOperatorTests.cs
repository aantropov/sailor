using SailorEditor.AI;
using SailorEditor.Commands;

namespace SailorEditor.Editor.Tests;

public class AIOperatorTests
{
    [Fact]
    public void EditorAIContextProvider_ShapesSelectionAndOriginMetadata()
    {
        var provider = new EditorAIContextProvider(new StubActionContextProvider(new ActionContext(
            ActiveWorldId: "MainWorld",
            ActiveSelectionIds: new[] { "go-1", "cmp-2" },
            ActivePanelId: "AI",
            ActiveDocumentId: "Scene",
            FocusedViewportId: "SceneViewport",
            CurrentAssetId: "asset-1",
            IsPlayMode: true,
            Origin: new CommandOrigin(CommandOriginKind.Panel, "AIPanel", "tester"))));

        var snapshot = provider.GetCurrentContext();

        Assert.Equal("MainWorld", snapshot.ActiveWorldId);
        Assert.Equal(2, snapshot.SelectionCount);
        Assert.Equal(new[] { "go-1", "cmp-2" }, snapshot.SelectionIds);
        Assert.Equal("AIPanel", snapshot.Metadata["originSource"]);
        Assert.Equal("tester", snapshot.Metadata["originActor"]);
    }

    [Fact]
    public async Task ExecuteProposalAsync_RejectsConfirmRequiredActionWithoutApproval()
    {
        var dispatcher = new RecordingDispatcher();
        var service = new AIOperatorService(
            new StaticPlanner(new AIPlanResponse(
                "create game object",
                "Need approval.",
                AIEditorContextSnapshot.Empty,
                new[] { new AIProposedAction("create-1", "Create GameObject", new[] { new StubCommand("Create") }, AIActionSafety.ConfirmRequired, "Create through command bus") })),
            new StaticAIContextProvider(),
            new StubActionContextProvider(new ActionContext()),
            dispatcher);

        await service.SubmitPromptAsync("create game object");
        var audit = await service.ExecuteProposalAsync("create-1");

        Assert.Equal(AIProposalState.Failed, audit.State);
        Assert.Empty(dispatcher.Commands);
        Assert.Equal("Approval required before execution.", audit.Summary);
    }

    [Fact]
    public async Task ExecuteProposalAsync_RoutesApprovedActionsThroughDispatcherWithAiOrigin()
    {
        var dispatcher = new RecordingDispatcher();
        var proposal = new AIProposedAction("open-1", "Open Console", new[] { new StubCommand("OpenPanel") }, AIActionSafety.SafeAutoExecute, "Open console panel");
        var service = new AIOperatorService(
            new StaticPlanner(new AIPlanResponse("open console", "Safe action ready.", AIEditorContextSnapshot.Empty, new[] { proposal })),
            new StaticAIContextProvider(),
            new StubActionContextProvider(new ActionContext(ActivePanelId: "AI")),
            dispatcher);

        await service.SubmitPromptAsync("open console");
        var audit = await service.ExecuteProposalAsync("open-1");

        Assert.Single(dispatcher.Commands);
        Assert.Equal(CommandOriginKind.AI, dispatcher.Contexts[0].Origin?.Kind);
        Assert.Equal("AIOperator", dispatcher.Contexts[0].Origin?.Source);
        Assert.Equal("open-1", dispatcher.Contexts[0].Metadata?["proposalId"]);
        Assert.Equal(AIProposalState.Executed, audit.State);
        Assert.Single(audit.Items);
        Assert.True(audit.Items[0].Succeeded);
    }

    sealed class StaticAIContextProvider : IAIEditorContextProvider
    {
        public AIEditorContextSnapshot GetCurrentContext() => AIEditorContextSnapshot.Empty;
    }

    sealed class StaticPlanner(AIPlanResponse response) : IAIActionPlanner
    {
        public Task<AIPlanResponse> PlanAsync(string prompt, AIEditorContextSnapshot context, CancellationToken cancellationToken = default) => Task.FromResult(response);
    }

    sealed class StubActionContextProvider(ActionContext context) : IActionContextProvider
    {
        public ActionContext GetCurrentContext(CommandOrigin? origin = null, IReadOnlyDictionary<string, string?>? metadata = null) => context with
        {
            Origin = origin ?? context.Origin,
            Metadata = metadata ?? context.Metadata,
        };
    }

    sealed class RecordingDispatcher : ICommandDispatcher
    {
        public List<IEditorCommand> Commands { get; } = new();
        public List<ActionContext> Contexts { get; } = new();

        public Task<CommandResult> DispatchAsync(IEditorCommand command, ActionContext context, CancellationToken cancellationToken = default)
        {
            Commands.Add(command);
            Contexts.Add(context);
            return Task.FromResult(CommandResult.Success(command.Name));
        }
    }

    sealed class StubCommand(string name) : IEditorCommand
    {
        public string Name { get; } = name;
        public bool CanExecute(ActionContext context) => true;
        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default) => Task.FromResult(CommandResult.Success());
    }
}
