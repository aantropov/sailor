using SailorEditor.Mcp;
using YamlDotNet.RepresentationModel;

namespace Editor.Tests;

public sealed class McpYamlUtilitiesTests
{
    [Fact]
    public void ToPlainObject_PreservesUnsignedInstanceIdAsString()
    {
        var value = McpYamlUtilities.ToPlainObject(
            new YamlScalarNode("17189606890707967961"));

        Assert.Equal("17189606890707967961", value);
    }

    [Theory]
    [InlineData("42", 42L)]
    [InlineData("1.5", 1.5)]
    [InlineData("true", true)]
    public void ToPlainObject_PreservesOrdinaryScalarTypes(
        string source,
        object expected)
    {
        var value = McpYamlUtilities.ToPlainObject(new YamlScalarNode(source));

        Assert.Equal(expected, value);
    }
}
