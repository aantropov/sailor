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
        Assert.Equal("Sailor::Component", Assert.Single(document.EngineTypes).Typename);
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
