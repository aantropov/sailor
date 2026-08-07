using SailorEditor.McpBridge;

namespace SailorEditor.Tests;

public sealed class McpBridgeOptionsTests
{
    [Fact]
    public void TryParse_ParsesExplicitEditorSelectors()
    {
        var parsed = McpBridgeOptions.TryParse(
            [
                "--pid",
                "42",
                "--workspace",
                "/tmp/SailorProject",
                "--discovery-directory",
                "/tmp/SailorMcp",
            ],
            out var options,
            out var error);

        Assert.True(parsed, error);
        Assert.Equal(42, options.ProcessId);
        Assert.Equal("/tmp/SailorProject", options.WorkspacePath);
        Assert.Equal("/tmp/SailorMcp", options.DiscoveryDirectory);
    }

    [Theory]
    [InlineData("--pid")]
    [InlineData("--pid", "zero")]
    [InlineData("--unknown", "value")]
    public void TryParse_RejectsInvalidArguments(params string[] arguments)
    {
        Assert.False(McpBridgeOptions.TryParse(
            arguments,
            out _,
            out var error));
        Assert.False(string.IsNullOrWhiteSpace(error));
    }
}
