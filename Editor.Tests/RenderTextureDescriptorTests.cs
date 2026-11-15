namespace SailorEditor.Tests;

using SailorEditor.Content;
using Xunit;
using YamlDotNet.RepresentationModel;

public class RenderTextureDescriptorTests
{
    [Fact]
    public void Parse_NativeEquivalentYaml_ReturnsCorrectDescriptor()
    {
        var yaml = "version: 1\nwidth: 1024\nheight: 768\nformat: R8G8B8A8_SRGB\n";
        var result = RenderTextureDescriptor.Parse(yaml);

        Assert.Equal(1024, result.Width);
        Assert.Equal(768, result.Height);
        Assert.Equal("R8G8B8A8_SRGB", result.Format);
    }

    [Theory]
    [InlineData("R8G8B8A8_UNORM")]
    [InlineData("R8G8B8A8_SRGB")]
    [InlineData("R16G16B16A16_SFLOAT")]
    public void RoundTrip_AllSupportedFormats_PreservesValues(string format)
    {
        var original = new RenderTextureDescriptor(256, 512, format);
        var serialized = original.Serialize();
        var parsed = RenderTextureDescriptor.Parse(serialized);

        Assert.Equal(original.Width, parsed.Width);
        Assert.Equal(original.Height, parsed.Height);
        Assert.Equal(original.Format, parsed.Format);
    }

    [Theory]
    [InlineData(1, 1)]
    [InlineData(16384, 16384)]
    [InlineData(1, 16384)]
    [InlineData(16384, 1)]
    public void RoundTrip_BoundaryDimensions_PreservesValues(int width, int height)
    {
        var original = new RenderTextureDescriptor(width, height);
        var serialized = original.Serialize();
        var parsed = RenderTextureDescriptor.Parse(serialized);

        Assert.Equal(width, parsed.Width);
        Assert.Equal(height, parsed.Height);
    }

    [Fact]
    public void Parse_MissingFormat_UsesDefaultUnorm()
    {
        var yaml = "version: 1\nwidth: 256\nheight: 256\n";
        var result = RenderTextureDescriptor.Parse(yaml);

        Assert.Equal("R8G8B8A8_UNORM", result.Format);
    }

    [Fact]
    public void Parse_Defaults_WhenAllOptional()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 512\n";
        var result = RenderTextureDescriptor.Parse(yaml);

        Assert.Equal(512, result.Width);
        Assert.Equal(512, result.Height);
        Assert.Equal("R8G8B8A8_UNORM", result.Format);
    }

    [Fact]
    public void Parse_UnknownExtraFields_Ignored()
    {
        var yaml = "version: 1\nwidth: 64\nheight: 64\nformat: R8G8B8A8_UNORM\nmipLevels: 4\nflags: 7\n";
        var result = RenderTextureDescriptor.Parse(yaml);

        Assert.Equal(64, result.Width);
        Assert.Equal(64, result.Height);
        Assert.Equal("R8G8B8A8_UNORM", result.Format);
    }

    [Fact]
    public void Parse_ZeroWidth_Throws()
    {
        var yaml = "version: 1\nwidth: 0\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_NegativeWidth_Throws()
    {
        var yaml = "version: 1\nwidth: -1\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_ZeroHeight_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 0\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_NegativeHeight_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: -1\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_Width16385_Throws()
    {
        var yaml = "version: 1\nwidth: 16385\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_Height16385_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 16385\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_FloatWidth_Throws()
    {
        var yaml = "version: 1\nwidth: 512.0\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_FloatHeight_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 512.5\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_OverflowWidth_Throws()
    {
        var yaml = "version: 1\nwidth: 99999999999999999999\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_MissingWidth_Throws()
    {
        var yaml = "version: 1\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_MissingHeight_Throws()
    {
        var yaml = "version: 1\nwidth: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_MissingVersion_Throws()
    {
        var yaml = "width: 512\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_FormatAsMap_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 512\nformat:\n  key: value\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_UnknownFormat_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 512\nformat: R32_UINT\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_WrongVersion_Throws()
    {
        var yaml = "version: 2\nwidth: 512\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_VersionZero_Throws()
    {
        var yaml = "version: 0\nwidth: 512\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_NonMapRoot_Throws()
    {
        var yaml = "- 512\n- 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_ScalarRoot_Throws()
    {
        var yaml = "hello\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_MultiDoc_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 512\n---\nversion: 1\nwidth: 256\nheight: 256\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_EmptyInput_Throws()
    {
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(""));
    }

    [Fact]
    public void Parse_WhitespaceInput_Throws()
    {
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse("   \n  \n"));
    }

    [Fact]
    public void Serialize_InvalidWidth_Throws()
    {
        var descriptor = new RenderTextureDescriptor(0, 512);
        Assert.Throws<InvalidOperationException>(() => descriptor.Serialize());
    }

    [Fact]
    public void Serialize_InvalidHeight_Throws()
    {
        var descriptor = new RenderTextureDescriptor(512, 16385);
        Assert.Throws<InvalidOperationException>(() => descriptor.Serialize());
    }

    [Fact]
    public void Serialize_InvalidFormat_Throws()
    {
        var descriptor = new RenderTextureDescriptor(512, 512, "R32_UINT");
        Assert.Throws<InvalidOperationException>(() => descriptor.Serialize());
    }

    [Fact]
    public void Validate_ValidDescriptor_DoesNotThrow()
    {
        var descriptor = new RenderTextureDescriptor(1024, 1024, "R16G16B16A16_SFLOAT");
        descriptor.Validate();
    }

    [Fact]
    public void Validate_OutOfRangeWidth_Throws()
    {
        var descriptor = new RenderTextureDescriptor(-5, 512);
        Assert.Throws<InvalidOperationException>(() => descriptor.Validate());
    }

    [Fact]
    public void Validate_OutOfRangeHeight_Throws()
    {
        var descriptor = new RenderTextureDescriptor(512, 0);
        Assert.Throws<InvalidOperationException>(() => descriptor.Validate());
    }

    [Fact]
    public void Validate_UnsupportedFormat_Throws()
    {
        var descriptor = new RenderTextureDescriptor(512, 512, "BC7_UNORM");
        Assert.Throws<InvalidOperationException>(() => descriptor.Validate());
    }

    [Fact]
    public void Serialize_OutputContainsAllFourFields()
    {
        var descriptor = new RenderTextureDescriptor(256, 128, "R8G8B8A8_SRGB");
        var output = descriptor.Serialize();

        var stream = new YamlStream();
        stream.Load(new StringReader(output));
        var root = stream.Documents[0].RootNode as YamlMappingNode;

        Assert.NotNull(root);
        Assert.Equal(4, root!.Children.Count);
        Assert.Equal("1", ((YamlScalarNode)root.Children[new YamlScalarNode("version")]).Value);
        Assert.Equal("256", ((YamlScalarNode)root.Children[new YamlScalarNode("width")]).Value);
        Assert.Equal("128", ((YamlScalarNode)root.Children[new YamlScalarNode("height")]).Value);
        Assert.Equal("R8G8B8A8_SRGB", ((YamlScalarNode)root.Children[new YamlScalarNode("format")]).Value);
    }

    [Fact]
    public void SupportedFormats_ContainsExactlyThreeEntries()
    {
        Assert.Equal(3, RenderTextureDescriptor.SupportedFormats.Count);
        Assert.Contains("R8G8B8A8_UNORM", RenderTextureDescriptor.SupportedFormats);
        Assert.Contains("R8G8B8A8_SRGB", RenderTextureDescriptor.SupportedFormats);
        Assert.Contains("R16G16B16A16_SFLOAT", RenderTextureDescriptor.SupportedFormats);
    }

    [Fact]
    public void Parse_VersionAsFloat_Throws()
    {
        var yaml = "version: 1.0\nwidth: 512\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_VersionAsMap_Throws()
    {
        var yaml = "version:\n  major: 1\nwidth: 512\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_WidthAsMap_Throws()
    {
        var yaml = "version: 1\nwidth:\n  px: 512\nheight: 512\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }

    [Fact]
    public void Parse_FormatAsSequence_Throws()
    {
        var yaml = "version: 1\nwidth: 512\nheight: 512\nformat:\n  - R8G8B8A8_UNORM\n";
        Assert.Throws<FormatException>(() => RenderTextureDescriptor.Parse(yaml));
    }
}
