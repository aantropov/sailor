namespace SailorEditor.Content;

using System.Globalization;
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

        if (stream.Documents.Count != 1)
        {
            throw new FormatException("RenderTextureDescriptor: expected a single document");
        }

        var root = stream.Documents[0].RootNode;
        if (root is not YamlMappingNode map)
        {
            throw new FormatException("RenderTextureDescriptor: expected a map");
        }

        if (!map.Children.TryGetValue(new YamlScalarNode("version"), out var versionNode))
        {
            throw new FormatException("RenderTextureDescriptor: missing 'version'");
        }

        if (versionNode is not YamlScalarNode versionScalar)
        {
            throw new FormatException("RenderTextureDescriptor: version must be a scalar");
        }

        if (!int.TryParse(versionScalar.Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int version))
        {
            throw new FormatException("RenderTextureDescriptor: version must be an integer");
        }

        if (version != 1)
        {
            throw new FormatException("RenderTextureDescriptor: unsupported version");
        }

        if (!map.Children.TryGetValue(new YamlScalarNode("width"), out var widthNode))
        {
            throw new FormatException("RenderTextureDescriptor: missing 'width'");
        }

        if (widthNode is not YamlScalarNode widthScalar)
        {
            throw new FormatException("RenderTextureDescriptor: width must be a scalar");
        }

        if (!int.TryParse(widthScalar.Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int width))
        {
            throw new FormatException("RenderTextureDescriptor: width must be an integer");
        }

        if (width < 1 || width > 16384)
        {
            throw new FormatException("RenderTextureDescriptor: width out of range");
        }

        if (!map.Children.TryGetValue(new YamlScalarNode("height"), out var heightNode))
        {
            throw new FormatException("RenderTextureDescriptor: missing 'height'");
        }

        if (heightNode is not YamlScalarNode heightScalar)
        {
            throw new FormatException("RenderTextureDescriptor: height must be a scalar");
        }

        if (!int.TryParse(heightScalar.Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out int height))
        {
            throw new FormatException("RenderTextureDescriptor: height must be an integer");
        }

        if (height < 1 || height > 16384)
        {
            throw new FormatException("RenderTextureDescriptor: height out of range");
        }

        string format = "R8G8B8A8_UNORM";

        if (map.Children.TryGetValue(new YamlScalarNode("format"), out var formatNode))
        {
            if (formatNode is not YamlScalarNode formatScalar)
            {
                throw new FormatException("RenderTextureDescriptor: format must be a scalar");
            }

            if (string.IsNullOrEmpty(formatScalar.Value))
            {
                throw new FormatException("RenderTextureDescriptor: format must not be empty");
            }

            if (!SupportedFormats.Contains(formatScalar.Value))
            {
                throw new FormatException("RenderTextureDescriptor: unsupported format string");
            }

            format = formatScalar.Value;
        }

        return new RenderTextureDescriptor(width, height, format);
    }

    public string Serialize()
    {
        Validate();

        var map = new YamlMappingNode();
        map.Children.Add(new YamlScalarNode("version"), new YamlScalarNode("1"));
        map.Children.Add(new YamlScalarNode("width"), new YamlScalarNode(Width.ToString(CultureInfo.InvariantCulture)));
        map.Children.Add(new YamlScalarNode("height"), new YamlScalarNode(Height.ToString(CultureInfo.InvariantCulture)));
        map.Children.Add(new YamlScalarNode("format"), new YamlScalarNode(Format));

        var doc = new YamlDocument(map);
        var stream = new YamlStream(doc);
        var writer = new StringWriter(CultureInfo.InvariantCulture);
        stream.Save(writer, false);
        return writer.ToString();
    }

    public void Validate()
    {
        if (Width < 1 || Width > 16384)
        {
            throw new InvalidOperationException("RenderTextureDescriptor: width out of range");
        }

        if (Height < 1 || Height > 16384)
        {
            throw new InvalidOperationException("RenderTextureDescriptor: height out of range");
        }

        if (!SupportedFormats.Contains(Format))
        {
            throw new InvalidOperationException("RenderTextureDescriptor: unsupported format");
        }
    }
}
