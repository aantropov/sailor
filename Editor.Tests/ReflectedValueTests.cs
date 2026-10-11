using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Collections.Specialized;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using YamlDotNet.Serialization;

namespace SailorEditor.Tests;

public class ReflectedValueTests
{
    const string ComponentName = "Sailor::Tests::RecordTestComponent";
    const string RecordName = "Sailor::Tests::RecordSettings";
    const string LayerName = "Sailor::Tests::RecordLayer";

    [Theory]
    [InlineData("{14a75bf9-09ac-49ee-a0df-2945f4b452a6}")]
    [InlineData("14a75bf9-09ac-49ee-a0df-2945f4b452a6")]
    [InlineData("14A75BF9-09AC-49EE-A0DF-2945F4B452A6")]
    public void AssetMetadataAndRuntimeFileIdsResolveTheSameEntry(string metadataId)
    {
        const string runtimeId = "14A75BF9-09AC-49EE-A0DF-2945F4B452A6";
        var fromMetadata = new FileId { Value = metadataId };
        var fromRuntime = new FileId(runtimeId);
        var assets = new Dictionary<FileId, string> { [fromMetadata] = "Duck.glb" };

        Assert.Equal(runtimeId, fromMetadata.Value);
        Assert.Equal(fromMetadata, fromRuntime);
        Assert.Equal(fromMetadata.GetHashCode(), fromRuntime.GetHashCode());
        Assert.Equal(0, fromMetadata.CompareTo(fromRuntime));
        Assert.Equal("Duck.glb", assets[fromRuntime]);
        Assert.Equal(runtimeId, ((FileId)fromMetadata.Clone()).Value);
        Assert.Equal(runtimeId, ((FileId)metadataId).Value);
    }

    [Theory]
    [InlineData(FileId.NullFileId)]
    [InlineData("")]
    [InlineData("{MODEL}")]
    [InlineData("Duck.glb")]
    public void NonGuidFileIdsKeepTheirText(string value)
    {
        Assert.Equal(value, new FileId(value).Value);
    }

    public static IEnumerable<object[]> Catalogs()
    {
        yield return ["fixture", CatalogYaml];
        if (Environment.GetEnvironmentVariable("SAILOR_RECORD_CATALOG") is { Length: > 0 } path)
            yield return ["native", File.ReadAllText(path)];
    }

    [Theory]
    [MemberData(nameof(Catalogs))]
    public void CatalogResolvesRecordsListsEnumsRangesAndDefaults(string source, string yaml)
    {
        Assert.NotEmpty(source);
        var catalog = EngineTypes.FromYaml(yaml);
        var list = Assert.IsType<ListProperty>(catalog.Components[ComponentName].Properties["records"]);
        var record = Assert.IsType<RecordProperty>(list.ElementType);
        Assert.Same(catalog.Components[RecordName], record.RecordType);
        Assert.IsType<Property<uint>>(record.RecordType.Properties["count"]);
        Assert.IsType<Property<int>>(record.RecordType.Properties["meshIndex"]);
        Assert.IsType<EnumProperty>(record.RecordType.Properties["mode"]);
        Assert.IsType<EnumProperty>(Assert.IsType<ListProperty>(record.RecordType.Properties["modes"]).ElementType);
        Assert.Equal(new NumericPropertyRange(0, 1), catalog.Components[LayerName].Properties["density"].Range);
        Assert.DoesNotContain("fileId", record.RecordType.Properties.Keys);
        Assert.DoesNotContain("instanceId", record.RecordType.Properties.Keys);
        Assert.DoesNotContain(RecordName, catalog.GetComponentTypeNames());
        Assert.Contains(ComponentName, catalog.GetComponentTypeNames());
        Assert.Contains("instanceId", catalog.Components[ComponentName].Properties.Keys);

        var codec = new ReflectedValueCodec(catalog);
        var value = Assert.IsType<ObservableRecord>(codec.CreateDefault(record));
        Assert.Equal(12u, Field<uint>(value, "count").Value);
        Assert.Equal(-1, Field<int>(value, "meshIndex").Value);
        Assert.Equal("Persistent", Field<string>(value, "mode").Value);
        Assert.Equal("default", Field<string>(value, "name").Value);
        var layer = Assert.IsType<ObservableRecord>(value.Fields["layer"]);
        Assert.Equal(0.5f, Field<float>(layer, "density").Value);
        Assert.True(Field<FileId>(layer, "materialFileId").Value.IsEmpty());
        Assert.Empty(Assert.IsType<ObservableValueList>(value.Fields["layers"]).Values);
        Assert.Equal(["Persistent", "Streamed"], Assert.IsType<ObservableValueList>(value.Fields["modes"])
            .Values.Cast<Observable<string>>().Select(mode => mode.Value));
    }

    [Theory]
    [MemberData(nameof(Catalogs))]
    public void EditingAndRoundTripKeepEveryRecordTogether(string source, string yaml)
    {
        Assert.NotEmpty(source);
        var catalog = EngineTypes.FromYaml(yaml);
        var descriptor = (ListProperty)catalog.Components[ComponentName].Properties["records"];
        var codec = new ReflectedValueCodec(catalog);
        var list = Assert.IsType<ObservableValueList>(Read(codec, descriptor, """
            - name: trees
              count: 37
              layer: { materialFileId: tree-material, density: 0.75 }
              layers: [{ materialFileId: leaves, density: 0.25 }]
            - name: 'water: grass'
              count: 1024
              meshIndex: 2
              mode: Streamed
              layer: { materialFileId: NullFileId, density: 0.125 }
              modes: [Streamed]
            """));
        list.Values.Move(1, 0);
        list.Values.RemoveAt(1);
        list.Values.Add(codec.CreateDefault(descriptor.ElementType));
        Field<uint>((ObservableRecord)list.Values[1], "count").Value = 99;

        var emitted = Write(codec, list);
        var loaded = Assert.IsType<ObservableValueList>(Read(codec, descriptor, emitted));
        Assert.Equal(2, loaded.Values.Count);
        var first = Assert.IsType<ObservableRecord>(loaded.Values[0]);
        Assert.Equal("water: grass", Field<string>(first, "name").Value);
        Assert.Equal(1024u, Field<uint>(first, "count").Value);
        Assert.Equal(2, Field<int>(first, "meshIndex").Value);
        Assert.Equal("Streamed", Field<string>(first, "mode").Value);
        Assert.Equal(0.125f, Field<float>((ObservableRecord)first.Fields["layer"], "density").Value);
        Assert.Equal(99u, Field<uint>((ObservableRecord)loaded.Values[1], "count").Value);
        Assert.Equal(12u, Field<uint>((ObservableRecord)codec.CreateDefault(descriptor.ElementType), "count").Value);
        Assert.Equal(emitted, Write(codec, loaded));

        list.Values.Clear();
        Assert.Empty(Assert.IsType<ObservableValueList>(Read(codec, descriptor, Write(codec, list))).Values);
        Assert.Empty(Assert.IsType<ObservableValueList>(Read(codec, descriptor, "null")).Values);
    }

    [Fact]
    public void ClonesOwnNestedRecordsListsIdsAndRotations()
    {
        var catalog = EngineTypes.FromYaml(CatalogYaml);
        var codec = new ReflectedValueCodec(catalog);
        var descriptor = catalog.Components[ComponentName].Properties["records"];
        var list = (ObservableValueList)Read(codec, descriptor, """
            - layer: { materialFileId: original }
              layers: [{ materialFileId: nested }]
            """);
        var original = (ObservableRecord)list.Values[0];
        original.Fields.Add("ids", new ObservableFileIdList([new FileId("asset")]));
        original.Fields.Add("weights", new ObservableFloatList([0.5f]));
        original.Fields.Add("target", new ObjectPtr { FileId = new FileId("target"), InstanceId = new InstanceId("instance") });
        original.Fields.Add("rotation", new Rotation(new Quat(0, 0, 0, 1)));
        var clone = (ObservableValueList)list.Clone();
        var copy = (ObservableRecord)clone.Values[0];
        Field<FileId>((ObservableRecord)copy.Fields["layer"], "materialFileId").Value.Value = "changed";
        Field<FileId>((ObservableRecord)((ObservableValueList)copy.Fields["layers"]).Values[0], "materialFileId").Value.Value = "changed";
        ((ObservableFileIdList)copy.Fields["ids"]).Values[0].Value.Value = "changed";
        ((ObservableFloatList)copy.Fields["weights"]).Values[0].Value = 2;
        ((ObjectPtr)copy.Fields["target"]).FileId.Value = "changed";
        ((ObjectPtr)copy.Fields["target"]).InstanceId.Value = "changed";
        ((Rotation)copy.Fields["rotation"]).Quat.X = 0.25f;
        ((ObservableValueList)copy.Fields["modes"]).Values.Clear();
        Assert.Equal("original", Field<FileId>((ObservableRecord)original.Fields["layer"], "materialFileId").Value.Value);
        Assert.Equal("nested", Field<FileId>((ObservableRecord)((ObservableValueList)original.Fields["layers"]).Values[0], "materialFileId").Value.Value);
        Assert.Equal("asset", ((ObservableFileIdList)original.Fields["ids"]).Values[0].Value.Value);
        Assert.Equal(0.5f, ((ObservableFloatList)original.Fields["weights"]).Values[0].Value);
        Assert.Equal("target", ((ObjectPtr)original.Fields["target"]).FileId.Value);
        Assert.Equal("instance", ((ObjectPtr)original.Fields["target"]).InstanceId.Value);
        Assert.Equal(0, ((Rotation)original.Fields["rotation"]).Quat.X);
        Assert.Equal(2, ((ObservableValueList)original.Fields["modes"]).Values.Count);
    }

    [Theory]
    [MemberData(nameof(Catalogs))]
    public void EveryCatalogDefaultRoundTrips(string source, string yaml)
    {
        Assert.NotEmpty(source);
        var catalog = EngineTypes.FromYaml(yaml);
        var codec = new ReflectedValueCodec(catalog);
        foreach (var type in catalog.Components.Values)
        {
            foreach (var property in type.Properties.Values)
            {
                var value = codec.CreateDefault(property);
                var encoded = Write(codec, value);
                Assert.Equal(encoded, Write(codec, Read(codec, property, encoded)));
            }
        }
    }

    [Theory]
    [InlineData("null")]
    [InlineData("true")]
    [InlineData("123")]
    [InlineData("name: value")]
    [InlineData("")]
    public void StringValuesKeepTheirText(string text)
    {
        var codec = new ReflectedValueCodec(new EngineTypes());
        var descriptor = new Property<string>();
        var decoded = Read(codec, descriptor, Write(codec, new Observable<string>(text)));
        Assert.Equal(text, Assert.IsType<Observable<string>>(decoded).Value);
    }

    [Fact]
    public void NestedEditsBubbleAndRemovedValuesAreUnsubscribed()
    {
        var catalog = EngineTypes.FromYaml(CatalogYaml);
        var codec = new ReflectedValueCodec(catalog);
        var descriptor = (ListProperty)catalog.Components[ComponentName].Properties["records"];
        var list = new ObservableValueList();
        var first = (ObservableRecord)codec.CreateDefault(descriptor.ElementType);
        var second = (ObservableRecord)codec.CreateDefault(descriptor.ElementType);
        list.Values.Add(first);
        list.Values.Add(second);
        var componentProperties = new ObservableDictionary<string, ObservableObject> { ["records"] = list };
        var changes = 0;
        componentProperties.ValueChanged += (_, _) => ++changes;
        var density = Field<float>((ObservableRecord)first.Fields["layer"], "density");
        density.Value = 0.6f;
        Assert.Equal(1, changes);
        list.Values.Move(0, 1);
        changes = 0;
        density.Value = 0.7f;
        Assert.Equal(1, changes);
        list.Values.Remove(first);
        changes = 0;
        density.Value = 0.8f;
        Assert.Equal(0, changes);
        list.Values[0] = first;
        changes = 0;
        Field<uint>(second, "count").Value = 45;
        Assert.Equal(0, changes);
        Field<uint>(first, "count").Value = 46;
        Assert.Equal(1, changes);
        list.Values.Clear();
        changes = 0;
        Field<uint>(first, "count").Value = 47;
        Assert.Equal(0, changes);
    }

    [Fact]
    public void DictionaryConstructorObservesInitialFieldsAndClearDetachesThem()
    {
        var value = new Observable<int>(1);
        var fields = new ObservableDictionary<string, ObservableObject>(
            new Dictionary<string, ObservableObject> { ["count"] = value });
        var changes = 0;
        fields.ValueChanged += (_, _) => ++changes;
        value.Value = 2;
        Assert.Equal(1, changes);
        ((IDictionary<string, ObservableObject>)fields).Clear();
        value.Value = 3;
        Assert.Equal(1, changes);
    }

    [Theory]
    [InlineData("count", "-1")]
    [InlineData("meshIndex", "1.5")]
    [InlineData("mode", "Unknown")]
    [InlineData("modes", "[Unknown]")]
    public void TypedFieldsRejectInvalidValues(string field, string yaml)
    {
        var catalog = EngineTypes.FromYaml(CatalogYaml);
        Assert.Throws<InvalidDataException>(() => Read(
            new ReflectedValueCodec(catalog), catalog.Components[RecordName].Properties[field], yaml));
    }

    [Theory]
    [InlineData("[{unknown: 1}]")]
    [InlineData("[3]")]
    [InlineData("{}")]
    public void RecordsRequireKnownFieldsAndListsRequireSequences(string yaml)
    {
        var catalog = EngineTypes.FromYaml(CatalogYaml);
        Assert.Throws<YamlException>(() => Read(new ReflectedValueCodec(catalog),
            catalog.Components[ComponentName].Properties["records"], yaml));
    }

    [Fact]
    public void ListsCanNestAndCreateIndependentDefaults()
    {
        var catalog = EngineTypes.FromYaml(CatalogYaml);
        var element = (ListProperty)catalog.Components[ComponentName].Properties["records"];
        var descriptor = new ListProperty { Typename = $"List<{element.Typename}>", ElementType = element };
        var codec = new ReflectedValueCodec(catalog);
        var value = (ObservableValueList)Read(codec, descriptor, "[[{count: 5}], [], null]");
        var restored = (ObservableValueList)Read(codec, descriptor, Write(codec, value));
        Assert.Equal(3, restored.Values.Count);
        Assert.Equal(5u, Field<uint>((ObservableRecord)((ObservableValueList)restored.Values[0]).Values[0], "count").Value);
        Assert.Empty(((ObservableValueList)restored.Values[1]).Values);
        Assert.Empty(((ObservableValueList)restored.Values[2]).Values);
    }

    static Observable<T> Field<T>(ObservableRecord record, string name) where T : IComparable<T> =>
        Assert.IsType<Observable<T>>(record.Fields[name]);

    static ObservableObject Read(ReflectedValueCodec codec, PropertyBase descriptor, string yaml) =>
        codec.Read(descriptor, new DeserializerBuilder().Build().Deserialize<object>(yaml), ComponentName, "records");

    static string Write(ReflectedValueCodec codec, ObservableObject value)
    {
        using var writer = new StringWriter();
        var emitter = new Emitter(writer);
        emitter.Emit(new StreamStart());
        emitter.Emit(new DocumentStart());
        codec.Write(emitter, value);
        emitter.Emit(new DocumentEnd(true));
        emitter.Emit(new StreamEnd());
        return writer.ToString();
    }

    const string CatalogYaml = """
        engineTypes:
          - typename: Sailor::Tests::RecordTestComponent
            base: Sailor::Component
            properties:
              records: List<Sailor::Tests::RecordSettings>
          - typename: Sailor::Tests::RecordSettings
            properties:
              name: string
              count: uint32
              meshIndex: int32
              mode: enum Sailor::Tests::ERecordMode
              layer: Sailor::Tests::RecordLayer
              layers: List<Sailor::Tests::RecordLayer>
              modes: List<enum Sailor::Tests::ERecordMode>
            propertyRanges:
              count: { min: 0, max: 2048 }
          - typename: Sailor::Tests::RecordLayer
            properties:
              materialFileId: FileId
              density: float
            propertyRanges:
              density: { min: 0, max: 1 }
        cdos:
          - typename: Sailor::Tests::RecordTestComponent
            defaultValues:
              records: []
          - typename: Sailor::Tests::RecordSettings
            defaultValues:
              name: default
              count: 12
              meshIndex: -1
              mode: Persistent
              layer: { materialFileId: NullFileId, density: 0.5 }
              layers: []
              modes: [Persistent, Streamed]
          - typename: Sailor::Tests::RecordLayer
            defaultValues:
              materialFileId: NullFileId
              density: 0.5
        enums:
          - enum Sailor::Tests::ERecordMode: [Persistent, Streamed]
        """;
}
