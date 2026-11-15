namespace SailorEditor.Content;

using System.Globalization;
using SailorEditor.Helpers;
using YamlDotNet.RepresentationModel;

public sealed record RenderTextureDescriptor(int Width = 512, int Height = 512, string Format = "R8G8B8A8_UNORM")
{
    public static readonly IReadOnlyList<string> SupportedFormats = Array.AsReadOnly(new[]
    {
        "R8G8B8A8_UNORM",
        "R8G8B8A8_SRGB",
        "R16G16B16A16_SFLOAT"
    });

    public static RenderTextureDescriptor Parse(string yaml)
    {
        if (string.IsNullOrWhiteSpace(yaml))
        {
            throw new FormatException("RenderTextureDescriptor: empty input");
        }

        var stream = new YamlStream();
        stream.Load(new StringReader(yaml));
        if (stream.Documents.Count != 1 || stream.Documents[0].RootNode is not YamlMappingNode map)
        {
            throw new FormatException("RenderTextureDescriptor: expected a single mapping document");
        }
        if (YamlHelper.ReadInt(map, "version") != 1)
        {
            throw new FormatException("RenderTextureDescriptor: expected version 1");
        }

        string format = "R8G8B8A8_UNORM";
        if (map.Children.ContainsKey(new YamlScalarNode("format")) &&
            !YamlHelper.TryGetScalar(map, "format", out format, requireNonWhitespace: true))
        {
            throw new FormatException("RenderTextureDescriptor: format must be a nonempty scalar");
        }

        var descriptor = new RenderTextureDescriptor(
            YamlHelper.ReadInt(map, "width"), YamlHelper.ReadInt(map, "height"), format);
        if (descriptor.GetValidationError() is { } error)
        {
            throw new FormatException("RenderTextureDescriptor: " + error);
        }
        return descriptor;
    }

    public string Serialize()
    {
        Validate();
        var map = new YamlMappingNode
        {
            { "version", YamlHelper.Scalar(1) },
            { "width", YamlHelper.Scalar(Width) },
            { "height", YamlHelper.Scalar(Height) },
            { "format", new YamlScalarNode(Format) }
        };
        var stream = new YamlStream(new YamlDocument(map));
        var writer = new StringWriter(CultureInfo.InvariantCulture);
        stream.Save(writer, false);
        return writer.ToString();
    }

    public void Validate()
    {
        if (GetValidationError() is { } error)
        {
            throw new InvalidOperationException("RenderTextureDescriptor: " + error);
        }
    }

    string? GetValidationError()
    {
        if (Width is < 1 or > 16384)
        {
            return "width out of range";
        }
        if (Height is < 1 or > 16384)
        {
            return "height out of range";
        }
        return SupportedFormats.Contains(Format) ? null : "unsupported format";
    }
}
