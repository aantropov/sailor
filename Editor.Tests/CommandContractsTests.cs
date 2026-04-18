using SailorEditor.Commands;

namespace SailorEditor.Editor.Tests;

public class CommandContractsTests
{
    [Fact]
    public async Task CommandResult_ReportsSuccessValue()
    {
        var command = new StubCommand();
        var context = new ActionContext(Origin: new CommandOrigin(CommandOriginKind.Test, "unit"));

        var result = await command.ExecuteAsync(context);

        Assert.True(result.Succeeded);
        Assert.Equal("ok", result.Message);
        Assert.Equal(CommandOriginKind.Test, context.Origin?.Kind);
    }

    private sealed class StubCommand : IEditorCommand
    {
        public string Name => "Stub";

        public bool CanExecute(ActionContext context) => true;

        public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
            => Task.FromResult(CommandResult.Success("ok"));
    }
}
