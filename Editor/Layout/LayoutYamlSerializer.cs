using SailorEditor.Utility;

namespace SailorEditor.Layout;

public static class LayoutYamlSerializer
{
    public static string Serialize(EditorLayout layout)
    {
        var serializer = SerializationUtils.CreateSerializerBuilder()
            .WithTagMapping("!split", typeof(SplitNode))
            .WithTagMapping("!tabs", typeof(TabGroupNode))
            .Build();

        return serializer.Serialize(layout);
    }

    public static EditorLayout Deserialize(string yaml)
    {
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTagMapping("!split", typeof(SplitNode))
            .WithTagMapping("!tabs", typeof(TabGroupNode))
            .Build();

        return deserializer.Deserialize<EditorLayout>(yaml) ?? LayoutOperations.CreateDefaultLayout();
    }
}
