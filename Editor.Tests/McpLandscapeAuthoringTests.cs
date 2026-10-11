using System.Text.Json;
using SailorEditor.Mcp;
using SailorEditor.Utility;
using SailorEngine;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization;

namespace SailorEditor.Tests;

public class McpLandscapeAuthoringTests
{
    const string ComponentName = "Sailor::LandscapeComponent";

    static McpLandscapeApplyRequest Request() => new()
    {
        MaterialFileId = "terrain",
        LayerTextureFileIds = ["sand", "grass", "rock", "snow"],
        Vegetation =
        [
            new() { ModelFileId = "tree", InstancesPerChunk = 37, Residency = "persistent", ColliderRadius = 0.5f },
            new() { ModelFileId = "grass", MaterialFileId = "leaves", MeshIndex = 3, InstancesPerChunk = 1024,
                Residency = "grass", ShadowMode = "all", ColliderRadius = 0.25f, ScreenCoverageThresholds = [0.5f, 0.1f, 0.01f] }
        ],
        Sculpt = [new() { X = 7, Z = 4, Radius = 8, Strength = 2, Operation = "flatten" }],
        Paint = [new() { X = -3, Layer = 3 }]
    };

    public static IEnumerable<object[]> Catalogs()
    {
        yield return ["fixture", CatalogYaml];
        if (Environment.GetEnvironmentVariable("SAILOR_RECORD_CATALOG") is { Length: > 0 } path)
            yield return ["native", File.ReadAllText(path)];
    }

    [Theory]
    [MemberData(nameof(Catalogs))]
    public void AuthoringOperationsRoundTripThroughTheReflectedRecordContract(string source, string yaml)
    {
        Assert.NotEmpty(source);
        var catalog = EngineTypes.FromYaml(yaml);
        var request = Request();
        Assert.True(McpLandscapeAuthoring.TryBuildOperations(request, out var operations, out var error), error);
        Assert.Equal(new[] { "create_game_object", "add_component", "select", "focus" }, operations.Select(operation => operation.Kind));
        var add = operations[1];
        Assert.Equal(ComponentName, add.ComponentType);
        var codec = new ReflectedValueCodec(catalog);
        var component = catalog.Components[ComponentName];
        var records = (ObservableValueList)Read(codec, component.Properties["vegetationProfiles"], add.Properties["vegetationProfiles"]);
        records.Values.Move(1, 0);
        records.Values.RemoveAt(1);
        var json = JsonSerializer.SerializeToElement(ReflectedValueCodec.ToPlainValue(records));
        var restored = (ObservableValueList)Read(codec, component.Properties["vegetationProfiles"], json);
        Assert.Single(restored.Values);
        var profile = JsonSerializer.SerializeToElement(ReflectedValueCodec.ToPlainValue(restored.Values[0]));
        var expected = JsonSerializer.SerializeToElement(request.Vegetation[1] with { Residency = "Grass", ShadowMode = "All" },
            new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });
        Assert.Equal(expected.EnumerateObject().Count(), profile.EnumerateObject().Count());
        foreach (var field in expected.EnumerateObject())
            Assert.Equal(field.Value.GetRawText(), profile.GetProperty(field.Name).GetRawText());

        var sculpt = Read(codec, component.Properties["sculptStamps"], add.Properties["sculptStamps"]);
        var sculptJson = JsonSerializer.SerializeToElement(ReflectedValueCodec.ToPlainValue(sculpt))[0];
        Assert.Equal("Flatten", sculptJson.GetProperty("operation").GetString());
        Assert.Equal(7, sculptJson.GetProperty("x").GetSingle());
        var paint = Read(codec, component.Properties["paintStamps"], add.Properties["paintStamps"]);
        Assert.Equal(3u, JsonSerializer.SerializeToElement(ReflectedValueCodec.ToPlainValue(paint))[0].GetProperty("layer").GetUInt32());

        var schemas = new McpComponentSchemaBuilder(catalog).Build();
        var schema = Assert.Single(schemas, entry => entry.Name == ComponentName);
        var vegetation = Assert.Single(schema.Properties, field => field.Name == "vegetationProfiles").Element!;
        Assert.Equal(new[] { "Persistent", "Grass" }, Assert.Single(vegetation.Fields!, field => field.Name == "residency").AllowedValues);
        Assert.IsType<uint>(Assert.Single(vegetation.Fields!, field => field.Name == "instancesPerChunk").DefaultValue);
        Assert.Equal(new NumericPropertyRange(0, 2048), Assert.Single(vegetation.Fields!, field => field.Name == "instancesPerChunk").Range);
        var coverage = Assert.Single(vegetation.Fields!, field => field.Name == "screenCoverageThresholds");
        Assert.Equal("List<float>", coverage.Type);
        Assert.Equal("float", coverage.Element!.Type);
    }

    [Fact]
    public void EditingAnExistingLandscapeOnlyUpdatesItsComponent()
    {
        Assert.True(McpLandscapeAuthoring.TryBuildOperations(Request() with { TargetComponentId = "existing" }, out var operations, out var error), error);
        var update = Assert.Single(operations);
        Assert.Equal("update_component", update.Kind);
        Assert.Equal("existing", update.Target);
        Assert.Equal(2, update.Properties["vegetationProfiles"].GetArrayLength());
    }

    [Theory]
    [InlineData("residency")]
    [InlineData("count")]
    [InlineData("coverage")]
    [InlineData("operation")]
    [InlineData("layer")]
    public void InvalidAuthoringDoesNotPublishPartialOperations(string field)
    {
        var request = Request();
        request = field switch
        {
            "residency" => request with { Vegetation = [request.Vegetation[0] with { Residency = "Unknown" }] },
            "count" => request with { Vegetation = [request.Vegetation[0] with { InstancesPerChunk = 2049 }] },
            "coverage" => request with { Vegetation = [request.Vegetation[0] with { ScreenCoverageThresholds = [0.1f, 0.9f] }] },
            "operation" => request with { Sculpt = [new() { Operation = "Unknown" }] },
            _ => request with { Paint = [new() { Layer = 4 }] }
        };
        Assert.False(McpLandscapeAuthoring.TryBuildOperations(request, out var operations, out var error));
        Assert.Empty(operations);
        Assert.NotEmpty(error);
    }

    static CommunityToolkit.Mvvm.ComponentModel.ObservableObject Read(ReflectedValueCodec codec, PropertyBase property, JsonElement json)
    {
        var mapping = new YamlMappingNode { { "value", McpYamlUtilities.FromJson(json) } };
        var values = new DeserializerBuilder().Build().Deserialize<Dictionary<string, object>>(McpYamlUtilities.Serialize(mapping));
        return codec.Read(property, values["value"], ComponentName, "value");
    }

    const string CatalogYaml = """
        engineTypes:
          - typename: Sailor::LandscapeComponent
            base: Sailor::Component
            properties:
              vegetationProfiles: List<Sailor::LandscapeVegetationSettings>
              sculptStamps: List<Sailor::LandscapeSculptStamp>
              paintStamps: List<Sailor::LandscapePaintStamp>
          - typename: Sailor::LandscapeVegetationSettings
            properties:
              modelFileId: FileId
              materialFileId: FileId
              meshIndex: int32
              instancesPerChunk: uint32
              residency: enum Sailor::ELandscapeVegetationResidency
              priority: float
              minScale: float
              maxScale: float
              groundOffset: float
              shadowMode: enum Sailor::ELandscapeVegetationShadowMode
              shadowDistance: float
              minLod: uint32
              maxLod: uint32
              screenCoverageThresholds: List<float>
              cullDistance: float
              colliderRadius: float
              colliderHeight: float
              colliderOffsetY: float
            propertyRanges:
              instancesPerChunk: { min: 0, max: 2048 }
          - typename: Sailor::LandscapeSculptStamp
            properties:
              x: float
              z: float
              radius: float
              strength: float
              operation: enum Sailor::ELandscapeSculptOperation
          - typename: Sailor::LandscapePaintStamp
            properties:
              x: float
              z: float
              radius: float
              strength: float
              layer: uint32
        enums:
          - enum Sailor::ELandscapeVegetationResidency: [Persistent, Grass]
          - enum Sailor::ELandscapeVegetationShadowMode: [None, NearOnly, All]
          - enum Sailor::ELandscapeSculptOperation: [Raise, Lower, Flatten]
        """;
}
