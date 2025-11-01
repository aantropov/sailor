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
using System.Windows.Input;
using CommunityToolkit.Mvvm.Input;
using System.IO;
using System.Threading.Tasks;

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

    public ModelFile()
    {
        AddMaterialCommand = new AsyncRelayCommand(OnAddMaterial);
        ClearMaterialCommand = new AsyncRelayCommand<Observable<FileId>>(OnClearMaterial);
        RemoveMaterialCommand = new Command<Observable<FileId>>(OnRemoveMaterial);
        ClearMaterialsCommand = new Command(OnClearMaterials);
    }

    public IAsyncRelayCommand AddMaterialCommand { get; }
    public ICommand RemoveMaterialCommand { get; }
    public ICommand ClearMaterialsCommand { get; }
    public IAsyncRelayCommand ClearMaterialCommand { get; }

    private async Task OnAddMaterial()
    {
        Materials.Add(new Observable<FileId>(default));
        await Task.CompletedTask;
    }

    private async Task OnClearMaterial(Observable<FileId> observable)
    {
        observable.Value = FileId.NullFileId;
        await Task.CompletedTask;
    }

    private void OnRemoveMaterial(Observable<FileId> material) => Materials.Remove(material);
    private void OnClearMaterials() => Materials.Clear();

    public override async Task Save() => await Save(new ModelFileYamlConverter());

    public override async Task Revert()
    {
        try
        {
            var yaml = await File.ReadAllTextAsync(AssetInfo.FullName);
            var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new ModelFileYamlConverter())
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
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new ObservableObjectYamlConverter<FileId>())
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