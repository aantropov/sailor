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

namespace SailorEditor.ViewModels;

public partial class ModelFile : AssetFile
{
    [ObservableProperty]
    bool shouldGenerateMaterials;

    [ObservableProperty]
    bool shouldBatchByMaterial;

    [ObservableProperty]
    bool shouldKeepCpuBuffers;

    [ObservableProperty]
    bool shouldGenerateBLAS = true;

    [ObservableProperty]
    float unitScale;

    [ObservableProperty]
    ObservableList<Observable<FileId>> materials = [];

    [ObservableProperty]
    ObservableList<Observable<FileId>> animations = [];

    public ModelFile()
    {
        AddMaterialCommand = new AsyncRelayCommand(OnAddMaterial);
        ClearMaterialCommand = new AsyncRelayCommand<Observable<FileId>>(OnClearMaterial);
        RemoveMaterialCommand = new Command<Observable<FileId>>(OnRemoveMaterial);
        ClearMaterialsCommand = new Command(OnClearMaterials);
        AddAnimationCommand = new AsyncRelayCommand(OnAddAnimation);
        ClearAnimationCommand = new AsyncRelayCommand<Observable<FileId>>(OnClearAnimation);
        RemoveAnimationCommand = new Command<Observable<FileId>>(OnRemoveAnimation);
        ClearAnimationsCommand = new Command(OnClearAnimations);
    }

    public IAsyncRelayCommand AddMaterialCommand { get; }
    public ICommand RemoveMaterialCommand { get; }
    public ICommand ClearMaterialsCommand { get; }
    public IAsyncRelayCommand ClearMaterialCommand { get; }
    public IAsyncRelayCommand AddAnimationCommand { get; }
    public ICommand RemoveAnimationCommand { get; }
    public ICommand ClearAnimationsCommand { get; }
    public IAsyncRelayCommand ClearAnimationCommand { get; }

    private Task OnAddMaterial()
    {
        Materials.Add(new Observable<FileId>(default));
        return Task.CompletedTask;
    }

    private Task OnClearMaterial(Observable<FileId> observable)
    {
        observable.Value = FileId.NullFileId;
        return Task.CompletedTask;
    }

    private void OnRemoveMaterial(Observable<FileId> material) => Materials.Remove(material);
    private void OnClearMaterials() => Materials.Clear();
    private Task OnAddAnimation()
    {
        Animations.Add(new Observable<FileId>(default));
        return Task.CompletedTask;
    }

    private Task OnClearAnimation(Observable<FileId> observable)
    {
        observable.Value = FileId.NullFileId;
        return Task.CompletedTask;
    }

    private void OnRemoveAnimation(Observable<FileId> animation) => Animations.Remove(animation);
    private void OnClearAnimations() => Animations.Clear();

    public override Task Save() => Save(new ModelFileYamlConverter());

    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();

                var yaml = File.ReadAllText(AssetInfo.FullName);
                var deserializer = SerializationUtils.CreateDeserializerBuilder()
                .WithTypeConverter(new ModelFileYamlConverter())
                .Build();

                var intermediateObject = deserializer.Deserialize<ModelFile>(yaml);

                FileId = intermediateObject.FileId ?? new FileId();
                Filename = intermediateObject.Filename ?? new FileId();
                ShouldGenerateMaterials = intermediateObject.ShouldGenerateMaterials;
                ShouldBatchByMaterial = intermediateObject.ShouldBatchByMaterial;
                ShouldKeepCpuBuffers = intermediateObject.ShouldKeepCpuBuffers;
                ShouldGenerateBLAS = intermediateObject.ShouldGenerateBLAS;
                UnitScale = intermediateObject.UnitScale;
                Materials = intermediateObject.Materials ?? [];
                Animations = intermediateObject.Animations ?? [];

                DisplayName = Asset.Name;

                Materials.CollectionChanged += (a, e) => MarkDirty(nameof(Materials));
                Materials.ItemChanged += (a, e) => MarkDirty(nameof(Materials));
                Animations.CollectionChanged += (a, e) => MarkDirty(nameof(Animations));
                Animations.ItemChanged += (a, e) => MarkDirty(nameof(Animations));

                IsLoaded = false;
            });
        }
        catch (Exception ex)
        {
            SetLoadError(ex);
        }

        ResetDirtyState();
        return Task.CompletedTask;
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
                    case "bShouldKeepCpuBuffers":
                        assetFile.ShouldKeepCpuBuffers = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bGenerateBLAS":
                        assetFile.ShouldGenerateBLAS = deserializer.Deserialize<bool>(parser);
                        break;
                    case "unitScale":
                        assetFile.UnitScale = deserializer.Deserialize<float>(parser);
                        break;
                    case "materials":
                        assetFile.Materials = deserializer.Deserialize<ObservableList<Observable<FileId>>>(parser);
                        break;
                    case "animations":
                        assetFile.Animations = deserializer.Deserialize<ObservableList<Observable<FileId>>>(parser);
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

        emitter.Emit(new Scalar(null, "bShouldKeepCpuBuffers"));
        emitter.Emit(new Scalar(null, assetFile.ShouldKeepCpuBuffers.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "bGenerateBLAS"));
        emitter.Emit(new Scalar(null, assetFile.ShouldGenerateBLAS.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "unitScale"));
        emitter.Emit(new Scalar(null, assetFile.UnitScale.ToString(CultureInfo.InvariantCulture)));

        emitter.Emit(new Scalar(null, "materials"));
        emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
        foreach (var material in assetFile.Materials)
            emitter.Emit(new Scalar(null, material.Value?.Value));
        emitter.Emit(new SequenceEnd());

        emitter.Emit(new Scalar(null, "animations"));
        emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
        foreach (var animation in assetFile.Animations)
            emitter.Emit(new Scalar(null, animation.Value?.Value));
        emitter.Emit(new SequenceEnd());

        emitter.Emit(new MappingEnd());
    }
}
