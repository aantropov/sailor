using System.Globalization;
using SailorEditor.Helpers;
using YamlDotNet.RepresentationModel;

namespace Editor.Tests;

public sealed class YamlHelperTests
{
    [Fact]
    public void TypedLookups_OnlyReturnMatchingNodeKinds()
    {
        var root = new YamlMappingNode
        {
            { "mapping", new YamlMappingNode { { "value", "nested" } } },
            { "sequence", new YamlSequenceNode("first", "second") },
            { "scalar", "value" }
        };

        Assert.True(YamlHelper.TryGetMapping(root, "mapping", out var mapping));
        Assert.Equal("nested", YamlHelper.ReadString(mapping, "value"));
        Assert.True(YamlHelper.TryGetSequence(root, "sequence", out var sequence));
        Assert.Equal(2, sequence.Children.Count);
        Assert.True(YamlHelper.TryGetScalar(root, "scalar", out var scalar));
        Assert.Equal("value", scalar);

        Assert.False(YamlHelper.TryGetMapping(root, "sequence", out _));
        Assert.False(YamlHelper.TryGetSequence(root, "scalar", out _));
        Assert.False(YamlHelper.TryGetScalar(root, "mapping", out _));
        Assert.False(YamlHelper.TryGetScalar(root, "missing", out _));
    }

    [Fact]
    public void ScalarReads_PreserveFallbackAndRequiredValuePolicies()
    {
        var root = new YamlMappingNode
        {
            { "empty", "" },
            { "whitespace", "   " },
            { "null", new YamlScalarNode(null) }
        };

        Assert.Equal("", YamlHelper.ReadString(root, "empty", "fallback"));
        Assert.Equal("fallback", YamlHelper.ReadString(root, "null", "fallback"));
        Assert.Equal("fallback", YamlHelper.ReadString(root, "missing", "fallback"));
        Assert.True(YamlHelper.TryGetScalar(root, "whitespace", out var whitespace));
        Assert.Equal("   ", whitespace);
        Assert.False(YamlHelper.TryGetScalar(
            root,
            "whitespace",
            out _,
            requireNonWhitespace: true));
    }

    [Fact]
    public void NumericReadsAndWrites_AreCultureInvariant()
    {
        var previousCulture = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR");
            var root = new YamlMappingNode
            {
                { "unsigned", "18446744073709551615" },
                { "integer", "-17" },
                { "float", "1.5" },
                { "bool", "true" },
                { "invalid", "not-a-number" }
            };

            Assert.Equal(ulong.MaxValue, YamlHelper.ReadUInt64(root, "unsigned"));
            Assert.Equal(-17, YamlHelper.ReadInt(root, "integer"));
            Assert.Equal(1.5f, YamlHelper.ReadFloat(root, "float"));
            Assert.True(YamlHelper.ReadBool(root, "bool"));
            Assert.Equal(23, YamlHelper.ReadInt(root, "invalid", 23));
            Assert.Equal(2.5f, YamlHelper.ReadFloat(root, "missing", 2.5f));

            Assert.Equal("18446744073709551615", YamlHelper.Scalar(ulong.MaxValue).Value);
            Assert.Equal("-17", YamlHelper.Scalar(-17).Value);
            Assert.Equal("1.5", YamlHelper.Scalar(1.5f).Value);
            Assert.Equal("true", YamlHelper.Scalar(true).Value);
        }
        finally
        {
            CultureInfo.CurrentCulture = previousCulture;
        }
    }
}
