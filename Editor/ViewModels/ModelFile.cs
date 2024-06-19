using YamlDotNet.RepresentationModel;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Globalization;
using SailorEngine;
using YamlDotNet.Serialization;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization.NamingConventions;
using Microsoft.Maui.Controls.Compatibility;
using SailorEditor.Services;

namespace SailorEditor.ViewModels;

public partial class ModelFile : AssetFile
{
    [ObservableProperty]
    bool shouldGenerateMaterials;

    [ObservableProperty]
    bool shouldBatchByMaterial;

    [ObservableProperty]
    float unitScale;

    [ObservableProperty]
    ObservableList<Observable<FileId>> materials = [];

    public override async Task Save() => await Save(new ModelFileYamlConverter());

    public override async Task Revert()
    {
        try
        {
            var yaml = File.ReadAllText(AssetInfo.FullName);
            var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new ModelFileYamlConverter())
            .IgnoreUnmatchedProperties()
            .Build();

            var intermediateObject = deserializer.Deserialize<ModelFile>(yaml);

            FileId = intermediateObject.FileId;
            Filename = intermediateObject.Filename;
            ShouldGenerateMaterials = intermediateObject.ShouldGenerateMaterials;
            ShouldBatchByMaterial = intermediateObject.ShouldBatchByMaterial;
            UnitScale = intermediateObject.UnitScale;
            Materials = intermediateObject.Materials;

            DisplayName = Asset.Name;

            Materials.CollectionChanged += (a, e) => MarkDirty(nameof(Materials));
            Materials.ItemChanged += (a, e) => MarkDirty(nameof(Materials));

            IsLoaded = false;
        }
        catch (Exception ex)
        {
            DisplayName = ex.Message;
        }

        IsDirty = false;
    }
}

public class ModelFileYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(ModelFile);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new ObservableListConverter<Observable<FileId>>(
                 [
                     new FileIdYamlConverter(),
                     new ObservableObjectYamlConverter<FileId>(),
                 ]))
            .IgnoreUnmatchedProperties()
            .Build();

        var assetFile = new ModelFile();

        parser.Consume<MappingStart>();

        while (parser.Current is not MappingEnd)
        {
            if (parser.Current is Scalar scalar)
            {
                var propertyName = scalar.Value;
                parser.MoveNext(); // Move to the value

                switch (propertyName)
                {
                    case "fileId":
                        assetFile.FileId = deserializer.Deserialize<FileId>(parser);
                        break;
                    case "filename":
                        assetFile.Filename = deserializer.Deserialize<string>(parser);
                        break;
                    case "bShouldGenerateMaterials":
                        assetFile.ShouldGenerateMaterials = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bShouldBatchByMaterial":
                        assetFile.ShouldBatchByMaterial = deserializer.Deserialize<bool>(parser);
                        break;
                    case "unitScale":
                        assetFile.UnitScale = deserializer.Deserialize<float>(parser);
                        break;
                    case "materials":
                        assetFile.Materials = deserializer.Deserialize<ObservableList<Observable<FileId>>>(parser);
                        break;
                    default:
                        deserializer.Deserialize<object>(parser);
                        break;
                }
            }
            else
            {
                parser.MoveNext();
            }
        }

        parser.Consume<MappingEnd>();

        return assetFile;
    }

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
        var assetFile = (ModelFile)value;

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        // Serialize inherited fields
        emitter.Emit(new Scalar(null, "fileId"));
        emitter.Emit(new Scalar(null, assetFile.FileId.Value));

        // Serialize ModelFile specific fields
        emitter.Emit(new Scalar(null, "filename"));
        emitter.Emit(new Scalar(null, assetFile.Filename));

        emitter.Emit(new Scalar(null, "bShouldGenerateMaterials"));
        emitter.Emit(new Scalar(null, assetFile.ShouldGenerateMaterials.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "bShouldBatchByMaterial"));
        emitter.Emit(new Scalar(null, assetFile.ShouldBatchByMaterial.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "unitScale"));
        emitter.Emit(new Scalar(null, assetFile.UnitScale.ToString(CultureInfo.InvariantCulture)));

        emitter.Emit(new Scalar(null, "materials"));
        emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
        foreach (var material in assetFile.Materials)
            emitter.Emit(new Scalar(null, material.Value?.Value));
        emitter.Emit(new SequenceEnd());

        emitter.Emit(new MappingEnd());
    }
}