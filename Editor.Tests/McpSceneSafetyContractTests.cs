namespace Editor.Tests;

public sealed class McpSceneSafetyContractTests
{
    [Fact]
    public void AddComponent_ValidatesPropertiesBeforeNativeMutation()
    {
        var source = ReadRepositoryFile("Editor", "Mcp", "McpSceneBatchExecutor.cs");
        var methodStart = source.IndexOf(
            "async Task<CommandResult> AddComponentAsync(",
            StringComparison.Ordinal);
        var methodEnd = source.IndexOf(
            "async Task<CommandResult> UpdateComponentAsync(",
            methodStart,
            StringComparison.Ordinal);

        Assert.True(methodStart >= 0);
        Assert.True(methodEnd > methodStart);

        var method = source[methodStart..methodEnd];
        var validate = method.IndexOf(
            "ValidateComponentPropertiesAsync(",
            StringComparison.Ordinal);
        var dispatch = method.IndexOf(
            "new AddComponentCommand(",
            StringComparison.Ordinal);

        Assert.True(validate >= 0);
        Assert.True(dispatch > validate);
    }

    [Fact]
    public void ObjectPointerProperties_RejectMalformedReferenceShapes()
    {
        var source = ReadRepositoryFile("Editor", "Mcp", "McpSceneBatchExecutor.cs");

        Assert.Contains("only accepts \" +", source, StringComparison.Ordinal);
        Assert.Contains("fileId and instanceId", source, StringComparison.Ordinal);
        Assert.Contains(".fileId' must be a string", source, StringComparison.Ordinal);
        Assert.Contains(".instanceId' must be a string", source, StringComparison.Ordinal);
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                Directory.Exists(Path.Combine(current.FullName, "Runtime")))
            {
                return File.ReadAllText(Path.Combine([current.FullName, .. relativePath]));
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
