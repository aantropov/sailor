using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using SailorEditor.Helpers;

namespace SailorEditor.Utility;

static class SerializationUtils
{
    static readonly IYamlTypeConverter[] DefaultConverters = new IYamlTypeConverter[]
    {
        new FileIdYamlConverter(),
        new InstanceIdYamlConverter(),
        new ObjectPtrYamlConverter(),
        new Vec2YamlConverter(),
        new Vec3YamlConverter(),
        new Vec4YamlConverter(),
        new QuatYamlConverter(),
        new RotationYamlConverter(),
    };

    public static SerializerBuilder CreateSerializerBuilder()
    {
        var builder = new SerializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .IncludeNonPublicProperties();
        foreach (var conv in DefaultConverters)
            builder.WithTypeConverter(conv);
        return builder;
    }

    public static DeserializerBuilder CreateDeserializerBuilder()
    {
        var builder = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .IgnoreUnmatchedProperties()
            .IncludeNonPublicProperties();
        foreach (var conv in DefaultConverters)
            builder.WithTypeConverter(conv);
        return builder;
    }
}
