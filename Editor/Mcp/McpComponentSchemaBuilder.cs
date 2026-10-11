#nullable enable

using SailorEditor.Utility;
using SailorEngine;

namespace SailorEditor.Mcp;

internal sealed class McpComponentSchemaBuilder(EngineTypes catalog)
{
    readonly ReflectedValueCodec codec = new(catalog);

    public IReadOnlyList<McpComponentTypeSchema> Build() => catalog.GetAddableComponentTypeNames()
        .Select(name => catalog.Components[name])
        .Select(type => new McpComponentTypeSchema(type.Name, type.Base,
            BuildProperties(type, new HashSet<string>(StringComparer.Ordinal) { type.Name })))
        .ToArray();

    IReadOnlyList<McpComponentPropertySchema> BuildProperties(ComponentType type, HashSet<string> path) =>
        type.Properties.OrderBy(property => property.Key, StringComparer.Ordinal)
            .Select(property => BuildProperty(property.Key, property.Value,
                type.ReadOnlyProperties.Contains(property.Key) || property.Key is "instanceId" or "fileId", path))
            .ToArray();

    McpComponentPropertySchema BuildProperty(string name, PropertyBase property, bool readOnly, HashSet<string> path)
    {
        IReadOnlyList<McpComponentPropertySchema>? fields = null;
        if (property is RecordProperty record && path.Add(record.RecordType.Name))
        {
            fields = BuildProperties(record.RecordType, path);
            path.Remove(record.RecordType.Name);
        }

        var elementType = property switch
        {
            ListProperty list => list.ElementType,
            Property<List<FileId>> => new FileIdProperty(),
            Property<List<float>> => new FloatProperty { Typename = "float" },
            _ => null
        };
        return new McpComponentPropertySchema(
            name, property.Typename, readOnly,
            ReflectedValueCodec.ToPlainValue(codec.CreateDefault(property)),
            property is EnumProperty ? catalog.Enums[property.Typename].ToArray() : null,
            property is ObjectPtrProperty pointer ? pointer.GenericTypename : null,
            property.Range, fields,
            elementType is not null ? BuildProperty("", elementType, false, path) : null);
    }
}
