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
        var range = Assert.Single(customType.PropertyRanges);
        Assert.Equal("moveSpeed", range.Key);
        Assert.Equal(0, range.Value.Min);
        Assert.Equal(20, range.Value.Max);
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
    public void Parse_RejectsEnumDefaultOutsideDeclaredValues()
    {
        var yaml = CombinedCatalogYaml.Replace("mode: Default", "mode: NotARealMode", StringComparison.Ordinal);

        var error = Assert.Throws<InvalidDataException>(() => EditorTypeCatalogSnapshot.Parse(yaml));

        Assert.Contains("NotARealMode", error.Message);
        Assert.Contains("SandboxLogic::SampleComponent.mode", error.Message);
    }

    [Fact]
    public void ComponentPropertyContract_RejectsEnumOverrideOutsideDeclaredValues()
    {
        var error = Assert.Throws<InvalidDataException>(() =>
            EditorComponentPropertyContract.ValidateEnumValue(
                "SandboxLogic::SampleComponent",
                "mode",
                "enum SandboxLogic::ESampleMode",
                "NotARealMode",
                ["Default", "Alternate"]));

        Assert.Contains("NotARealMode", error.Message);
        Assert.Contains("SandboxLogic::SampleComponent.mode", error.Message);
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

    [Fact]
    public void AddComponentQuery_HidesEditorAndTestHarnessTypesWithoutRemovingThemFromCatalog()
    {
        const string yaml = """
engineTypes:
  - typename: Sailor::IReflectable
    base: ''
  - typename: Sailor::Component
    base: Sailor::IReflectable
  - typename: Sailor::MeshRendererComponent
    base: Sailor::Component
  - typename: Sailor::EditorComponent
    base: Sailor::Component
  - typename: Sailor::TestComponent
    base: Sailor::Component
  - typename: Sailor::TestCaseComponent
    base: Sailor::Component
  - typename: Sailor::VisualTestCaseComponent
    base: Sailor::TestCaseComponent
  - typename: SandboxLogic::DerivedVisualTestComponent
    base: Sailor::VisualTestCaseComponent
  - typename: Sailor::PerformanceTestSetupComponent
    base: Sailor::Component
  - typename: SandboxLogic::ContestComponent
    base: Sailor::Component
  - typename: SandboxLogic::EditorComponent
    base: Sailor::Component
cdos: []
enums: []
assetTypes: []
""";

        var snapshot = EditorTypeCatalogSnapshot.Parse(yaml);

        Assert.True(snapshot.TryGetType("Sailor::EditorComponent", out _));
        Assert.True(snapshot.TryGetType("Sailor::VisualTestCaseComponent", out _));
        Assert.True(snapshot.IsComponentType("Sailor::EditorComponent"));
        Assert.True(snapshot.IsComponentType("Sailor::VisualTestCaseComponent"));
        Assert.Contains("Sailor::EditorComponent", snapshot.GetComponentTypeNames());
        Assert.Contains("Sailor::VisualTestCaseComponent", snapshot.GetComponentTypeNames());

        Assert.Equal(
            [
                "Sailor::MeshRendererComponent",
                "SandboxLogic::ContestComponent",
                "SandboxLogic::EditorComponent"
            ],
            snapshot.GetAddableComponentTypeNames());
        Assert.False(snapshot.IsAddableComponentType("Sailor::EditorComponent"));
        Assert.False(snapshot.IsAddableComponentType("Sailor::TestComponent"));
        Assert.False(snapshot.IsAddableComponentType("Sailor::TestCaseComponent"));
        Assert.False(snapshot.IsAddableComponentType("Sailor::VisualTestCaseComponent"));
        Assert.False(snapshot.IsAddableComponentType("SandboxLogic::DerivedVisualTestComponent"));
        Assert.False(snapshot.IsAddableComponentType("Sailor::PerformanceTestSetupComponent"));
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
    public void Parse_AcceptsRangesForSupportedNumericProperties()
    {
        const string yaml = """
engineTypes:
  - typename: Sailor::Component
    base: Sailor::IReflectable
    properties:
      opacity: float
      priority: int32
      samples: uint32
    propertyRanges:
      opacity: { min: 0, max: 1 }
      priority: { min: -10, max: 10 }
      samples: { min: 1, max: 64 }
cdos: []
enums: []
assetTypes: []
""";

        var type = Assert.Single(EditorTypeCatalogSnapshot.Parse(yaml).Document.EngineTypes);

        Assert.Equal(3, type.PropertyRanges.Count);
        Assert.Equal(-10, type.PropertyRanges["priority"].Min);
        Assert.Equal(64, type.PropertyRanges["samples"].Max);
    }

    [Fact]
    public void Parse_RejectsExplicitNullPropertyRanges()
    {
        const string yaml = """
engineTypes:
  - typename: Sailor::Component
    base: Sailor::IReflectable
    properties:
      opacity: float
    propertyRanges: null
cdos: []
enums: []
assetTypes: []
""";

        var error = Assert.Throws<InvalidDataException>(() => EditorTypeCatalogSnapshot.Parse(yaml));

        Assert.Contains("must declare propertyRanges as a map", error.Message);
    }

    [Fact]
    public void Parse_RejectsUnknownPropertyRangeFields()
    {
        const string yaml = """
engineTypes:
  - typename: Sailor::Component
    base: Sailor::IReflectable
    properties:
      opacity: float
    propertyRanges:
      opacity: { min: 0, max: 1, step: 0.1 }
cdos: []
enums: []
assetTypes: []
""";

        var error = Assert.Throws<InvalidDataException>(() => EditorTypeCatalogSnapshot.Parse(yaml));

        Assert.Contains("must contain exactly scalar min and max fields", error.Message);
    }

    [Theory]
    [InlineData(
        "missing",
        "float",
        "known: { min: 0, max: 1 }",
        "does not reference a reflected writable property")]
    [InlineData(
        "known",
        "bool",
        "known: { min: 0, max: 1 }",
        "cannot be applied to 'bool'")]
    [InlineData(
        "known",
        "float",
        "known: { min: 1, max: 1 }",
        "min less than max")]
    [InlineData(
        "known",
        "int32",
        "known: { min: 0.5, max: 10 }",
        "representable integral int32 bounds")]
    [InlineData(
        "known",
        "uint32",
        "known: { min: -1, max: 10 }",
        "representable integral uint32 bounds")]
    public void Parse_RejectsInvalidPropertyRanges(
        string propertyName,
        string propertyType,
        string rangeYaml,
        string expectedError)
    {
        var yaml = $$"""
engineTypes:
  - typename: Sailor::Component
    base: Sailor::IReflectable
    properties:
      {{propertyName}}: {{propertyType}}
    propertyRanges:
      {{rangeYaml}}
cdos: []
enums: []
assetTypes: []
""";

        var error = Assert.Throws<InvalidDataException>(() => EditorTypeCatalogSnapshot.Parse(yaml));

        Assert.Contains(expectedError, error.Message);
    }

    [Fact]
    public void NumericPropertyRange_ClampsOnlyWhenAnExplicitEditUsesIt()
    {
        var range = new NumericPropertyRange(0, 10);
        var loadedValue = 25.0f;

        var sliderDisplayValue = range.Clamp(loadedValue);

        Assert.Equal(10, sliderDisplayValue);
        Assert.Equal(25.0f, loadedValue);
        Assert.Equal(0.0f, range.Clamp(-5.0f));
        Assert.Equal(10, range.Clamp(15));
        Assert.Equal(0u, range.Clamp(0u));
        Assert.Equal(4, range.SnapInt32(3.6));
        Assert.Equal(4u, range.SnapUInt32(3.6));
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
    propertyRanges:
      moveSpeed: { min: 0, max: 20 }
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
