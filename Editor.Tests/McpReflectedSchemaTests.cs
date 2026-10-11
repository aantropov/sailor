using System.Text.Json;
using SailorEditor.Mcp;
using SailorEditor.Utility;
using SailorEngine;

namespace SailorEditor.Tests;

public class McpReflectedSchemaTests
{
    [Theory]
    [MemberData(nameof(ReflectedValueTests.Catalogs), MemberType = typeof(ReflectedValueTests))]
    public void SchemasDescribeNestedRecordsListsAndTypedDefaults(string source, string yaml)
    {
        Assert.NotEmpty(source);
        var catalog = EngineTypes.FromYaml(yaml);
        var schemas = new McpComponentSchemaBuilder(catalog).Build();
        var component = Assert.Single(schemas, schema => schema.Name == "Sailor::Tests::RecordTestComponent");
        var list = Assert.Single(component.Properties, field => field.Name == "records");
        Assert.Empty(Assert.IsType<object[]>(list.DefaultValue));
        var record = Assert.IsType<McpComponentPropertySchema>(list.Element);
        Assert.Equal("Sailor::Tests::RecordSettings", record.Type);
        var count = Assert.Single(record.Fields!, field => field.Name == "count");
        Assert.Equal("uint32", count.Type);
        Assert.Equal(12u, Assert.IsType<uint>(count.DefaultValue));
        Assert.Equal(new NumericPropertyRange(0, 2048), count.Range);
        Assert.Equal(-1, Assert.Single(record.Fields!, field => field.Name == "meshIndex").DefaultValue);
        var mode = Assert.Single(record.Fields!, field => field.Name == "mode");
        Assert.Equal(new[] { "Persistent", "Streamed" }, mode.AllowedValues);
        Assert.Equal("Persistent", mode.DefaultValue);
        var layer = Assert.Single(record.Fields!, field => field.Name == "layer");
        Assert.Equal(new NumericPropertyRange(0, 1), Assert.Single(layer.Fields!, field => field.Name == "density").Range);
        var layers = Assert.Single(record.Fields!, field => field.Name == "layers");
        Assert.Equal(layer.Fields, layers.Element!.Fields);
        Assert.DoesNotContain(record.Fields!, field => field.Name is "fileId" or "instanceId");
        Assert.True(Assert.Single(component.Properties, field => field.Name == "instanceId").ReadOnly);

        var defaults = JsonSerializer.SerializeToElement(record.DefaultValue);
        Assert.Equal(12u, defaults.GetProperty("count").GetUInt32());
        Assert.Equal("NullFileId", defaults.GetProperty("layer").GetProperty("materialFileId").GetString());
        Assert.Equal("Streamed", defaults.GetProperty("modes")[1].GetString());
    }

    [Theory]
    [InlineData("null")]
    [InlineData("true")]
    [InlineData("123")]
    [InlineData("")]
    public void SchemaProjectionKeepsStringDefaultsAsStrings(string text)
    {
        var codec = new ReflectedValueCodec(new EngineTypes());
        var value = ReflectedValueCodec.ToPlainValue(codec.CreateDefault(new Property<string> { DefaultValue = text }));
        var json = JsonSerializer.SerializeToElement(value);
        Assert.Equal(JsonValueKind.String, json.ValueKind);
        Assert.Equal(text, json.GetString());
    }

    [Fact]
    public void RecursiveRecordListsUseTheTypeNameWithoutExpandingForever()
    {
        var record = new ComponentType { Name = "Tree", Base = "" };
        var element = new RecordProperty { Typename = "Tree", RecordType = record };
        record.Properties.Add("children", new ListProperty { Typename = "List<Tree>", ElementType = element });
        var catalog = new EngineTypes();
        catalog.Components.Add("Tree", record);
        catalog.Components.Add("TreeComponent", new ComponentType
        {
            Name = "TreeComponent", Base = "Sailor::Component", Properties = new() { ["root"] = element }
        });
        var root = Assert.Single(Assert.Single(new McpComponentSchemaBuilder(catalog).Build()).Properties);
        var child = Assert.Single(root.Fields!).Element!;
        Assert.Equal("Tree", child.Type);
        Assert.Null(child.Fields);
        Assert.Empty(Assert.IsType<object[]>(Assert.IsType<Dictionary<string, object>>(child.DefaultValue)["children"]));
    }
}
