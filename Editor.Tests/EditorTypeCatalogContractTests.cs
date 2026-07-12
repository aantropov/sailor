using System.Globalization;
using SailorEngine;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.Editor.Tests;

public sealed class EditorTypeCatalogContractTests
{
    [Fact]
    public void Parse_CombinedCatalogPreservesCustomFullyQualifiedType()
    {
        var snapshot = EditorTypeCatalogSnapshot.Parse(CombinedCatalogYaml);

        Assert.True(snapshot.TryGetType("SandboxLogic::SampleComponent", out var customType));
        Assert.Equal("Sailor::Component", customType.Base);
        Assert.Equal("float", customType.Properties["moveSpeed"]);
        Assert.Equal("enum SandboxLogic::ESampleMode", customType.Properties["mode"]);
        Assert.True(snapshot.IsKnownReadOnlyProperty(
            "SandboxLogic::SampleComponent",
            "readOnlyValue"));
        Assert.True(snapshot.IsKnownReadOnlyProperty(
            "SandboxLogic::SampleComponent",
            "skippedReadOnlyValue"));
        Assert.False(snapshot.IsKnownReadOnlyProperty(
            "SandboxLogic::SampleComponent",
            "moveSpeed"));
        Assert.True(snapshot.IsComponentType("SandboxLogic::SampleComponent"));
        Assert.Equal(
            ["SandboxLogic::SampleComponent"],
            snapshot.GetComponentTypeNames());

        var defaults = snapshot.Document.Cdos.Single(x => x.Typename == "SandboxLogic::SampleComponent");
        Assert.Equal(
            [0.0f, 0.25f, 0.5f, 1.0f],
            EditorTypeMetadataValueCodec.ParseFloatSequence(
                defaults.DefaultValues["orientation"],
                4,
                "SandboxLogic::SampleComponent.orientation"));
        Assert.Equal(
            ["Default", "Alternate"],
            Assert.Single(snapshot.Document.Enums)["enum SandboxLogic::ESampleMode"]);
    }

    [Fact]
    public void Parse_RejectsPropertyWhoseEnumMetadataIsMissing()
    {
        const string yaml = """
engineTypes:
  - typename: Sailor::Component
    base: Sailor::IReflectable
    properties: {}
  - typename: SandboxLogic::SampleComponent
    base: Sailor::Component
    properties:
      mode: enum SandboxLogic::ESampleMode
cdos: []
enums: []
assetTypes: []
""";

        var error = Assert.Throws<InvalidDataException>(() => EditorTypeCatalogSnapshot.Parse(yaml));

        Assert.Contains("missing enum metadata 'enum SandboxLogic::ESampleMode'", error.Message);
    }

    [Fact]
    public void ComponentPropertyContract_DistinguishesWritableReadOnlyAndUnknownProperties()
    {
        var writable = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["moveSpeed"] = "float"
        };
        var readOnly = new HashSet<string>(StringComparer.Ordinal)
        {
            "readOnlyValue",
            "skippedReadOnlyValue"
        };

        Assert.Equal(
            EditorComponentPropertyAccess.Writable,
            EditorComponentPropertyContract.Classify("moveSpeed", writable, readOnly));
        Assert.Equal(
            EditorComponentPropertyAccess.ReadOnly,
            EditorComponentPropertyContract.Classify("readOnlyValue", writable, readOnly));
        Assert.Equal(
            EditorComponentPropertyAccess.ReadOnly,
            EditorComponentPropertyContract.Classify("skippedReadOnlyValue", writable, readOnly));
        Assert.Equal(
            EditorComponentPropertyAccess.Unknown,
            EditorComponentPropertyContract.Classify("typo", writable, readOnly));
    }

    [Fact]
    public void Parse_RejectsDuplicateTypeWithoutReturningPartialCatalog()
    {
        const string yaml = """
engineTypes:
  - typename: Sailor::Component
    base: Sailor::IReflectable
  - typename: Sailor::Component
    base: Sailor::IReflectable
cdos: []
enums: []
assetTypes: []
""";

        var error = Assert.Throws<InvalidDataException>(() => EditorTypeCatalogSnapshot.Parse(yaml));

        Assert.Contains("Duplicate type 'Sailor::Component'", error.Message);
    }

    [Fact]
    public void ComponentQuery_HandlesMissingBaseAndCycles()
    {
        const string yaml = """
engineTypes:
  - typename: MissingBaseComponent
    base: DoesNotExist
  - typename: CycleA
    base: CycleB
  - typename: CycleB
    base: CycleA
cdos: []
enums: []
assetTypes: []
""";

        var snapshot = EditorTypeCatalogSnapshot.Parse(yaml);

        Assert.False(snapshot.IsComponentType("MissingBaseComponent"));
        Assert.False(snapshot.IsComponentType("CycleA"));
        Assert.False(snapshot.IsComponentType("CycleB"));
        Assert.False(snapshot.IsComponentType("UnknownComponent"));
        Assert.Empty(snapshot.GetComponentTypeNames());
    }

    [Theory]
    [InlineData(EditorComponentScalarKind.String, "true: still text")]
    [InlineData(EditorComponentScalarKind.Boolean, "true")]
    [InlineData(EditorComponentScalarKind.Int32, "-42")]
    [InlineData(EditorComponentScalarKind.UInt32, "42")]
    [InlineData(EditorComponentScalarKind.Float, "3.125")]
    public void ScalarCodec_RoundTripsWithoutCurrentCultureLoss(
        EditorComponentScalarKind kind,
        string serializedValue)
    {
        var previousCulture = CultureInfo.CurrentCulture;
        try
        {
            CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("fr-FR");
            var value = EditorComponentScalarCodec.Parse(kind, serializedValue);
            var roundTrip = EditorComponentScalarCodec.Format(kind, value);

            Assert.Equal(serializedValue, roundTrip);
        }
        finally
        {
            CultureInfo.CurrentCulture = previousCulture;
        }
    }

    [Fact]
    public void ScalarCodec_RejectsInvalidPrimitiveInsteadOfSubstitutingDefault()
    {
        var error = Assert.Throws<InvalidDataException>(() =>
            EditorComponentScalarCodec.Parse(EditorComponentScalarKind.Int32, "not-an-int"));

        Assert.Contains("not-an-int", error.Message);
    }

    [Fact]
    public void ScalarCodec_RejectsNegativeUnsignedValue()
    {
        Assert.Throws<InvalidDataException>(() =>
            EditorComponentScalarCodec.Parse(EditorComponentScalarKind.UInt32, "-1"));
    }

    [Fact]
    public void MetadataValueCodec_RejectsMalformedSequence()
    {
        Assert.Throws<InvalidDataException>(() =>
            EditorTypeMetadataValueCodec.ParseFloatSequence(new object[] { 1, "invalid" }, 2, "rotation"));
        Assert.Throws<InvalidDataException>(() =>
            EditorTypeMetadataValueCodec.ParseFloatSequence(new object[] { 1, 2, 3 }, 4, "rotation"));
    }

    [Fact]
    public void ComponentContract_PreservesFqnAndScalarsRegardlessOfKeyOrder()
    {
        const string yaml = """
overrideProperties:
  moveSpeed: 12.5
  enabled: true
  retries: 3
  readOnlyValue: 17
  skippedReadOnlyValue: 23
typename: SandboxLogic::SampleComponent
""";
        var serializer = new SerializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .Build();
        var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .Build();

        var document = deserializer.Deserialize<EditorComponentYamlContract>(yaml);
        var roundTrip = deserializer.Deserialize<EditorComponentYamlContract>(serializer.Serialize(document));

        Assert.Equal("SandboxLogic::SampleComponent", roundTrip.Typename);
        Assert.Equal(12.5f, (float)EditorComponentScalarCodec.Parse(
            EditorComponentScalarKind.Float,
            Convert.ToString(roundTrip.OverrideProperties["moveSpeed"], CultureInfo.InvariantCulture)));
        Assert.True((bool)EditorComponentScalarCodec.Parse(
            EditorComponentScalarKind.Boolean,
            Convert.ToString(roundTrip.OverrideProperties["enabled"], CultureInfo.InvariantCulture)));
        Assert.Equal(3, (int)EditorComponentScalarCodec.Parse(
            EditorComponentScalarKind.Int32,
            Convert.ToString(roundTrip.OverrideProperties["retries"], CultureInfo.InvariantCulture)));
        Assert.Equal(17, Convert.ToInt32(
            roundTrip.OverrideProperties["readOnlyValue"],
            CultureInfo.InvariantCulture));
        Assert.Equal(23, Convert.ToInt32(
            roundTrip.OverrideProperties["skippedReadOnlyValue"],
            CultureInfo.InvariantCulture));
    }

    const string CombinedCatalogYaml = """
metadataVersion: 1
moduleName: SailorEditor
engineTypes:
  - typename: Sailor::IReflectable
    base: ''
    properties: {}
  - typename: Sailor::Component
    base: Sailor::IReflectable
    properties: {}
  - typename: SandboxLogic::SampleComponent
    base: Sailor::Component
    properties:
      moveSpeed: float
      orientation: struct glm::qua<float,0>
      mode: enum SandboxLogic::ESampleMode
    readOnlyProperties: [readOnlyValue, skippedReadOnlyValue]
cdos:
  - typename: SandboxLogic::SampleComponent
    defaultValues:
      moveSpeed: 5
      orientation: [0, 0.25, 0.5, 1]
      mode: Default
      readOnlyValue: 17
enums:
  - enum SandboxLogic::ESampleMode: [Default, Alternate]
assetTypes: []
""";
}
