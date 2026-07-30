using SailorEngine;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.Editor.Tests;

public sealed class EngineTypeMetadataContractTests
{
    [Fact]
    public void Deserialize_LegacyEngineMetadataKeepsOptionalIdentityEmpty()
    {
        var document = Deserialize("""
timeStamp: 42
engineTypes:
  - typename: Sailor::Component
    base: Sailor::IReflectable
    properties: {}
cdos: []
enums: []
assetTypes: []
""");

        Assert.Null(document.MetadataVersion);
        Assert.Null(document.ModuleName);
        Assert.Equal(42, document.TimeStamp);
        var type = Assert.Single(document.EngineTypes);
        Assert.Equal("Sailor::Component", type.Typename);
        Assert.Empty(type.PropertyRanges);
    }

    [Fact]
    public void Deserialize_WorkspaceMetadataReadsIdentityTypeAndDefaults()
    {
        var document = Deserialize("""
metadataVersion: 1
moduleName: SandboxLogic
timeStamp: 84
engineTypes:
  - typename: SandboxLogic::SampleComponent
    base: Sailor::Component
    properties:
      moveSpeed: float
    propertyRanges:
      moveSpeed:
        min: 0
        max: 20
cdos:
  - typename: SandboxLogic::SampleComponent
    defaultValues:
      moveSpeed: 5
enums: []
assetTypes: []
""");

        Assert.Equal(1u, document.MetadataVersion);
        Assert.Equal("SandboxLogic", document.ModuleName);
        var type = Assert.Single(document.EngineTypes);
        Assert.Equal("float", type.Properties["moveSpeed"]);
        var range = Assert.Single(type.PropertyRanges);
        Assert.Equal("moveSpeed", range.Key);
        Assert.Equal(0, range.Value.Min);
        Assert.Equal(20, range.Value.Max);
        var defaults = Assert.Single(document.Cdos);
        Assert.True(defaults.DefaultValues.ContainsKey("moveSpeed"));
    }

    static EngineTypeMetadataContract Deserialize(string yaml)
        => new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .IgnoreUnmatchedProperties()
            .Build()
            .Deserialize<EngineTypeMetadataContract>(yaml);
}
