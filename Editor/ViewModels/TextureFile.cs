using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEngine;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using SailorEditor.Services;
using YamlDotNet.Core.Tokens;
using Scalar = YamlDotNet.Core.Events.Scalar;

namespace SailorEditor.ViewModels;

public partial class TextureFile : AssetFile
{
    public bool IsImageLoaded { get => !Texture.IsEmpty; }

    [ObservableProperty]
    List<string> textureFiltrationType = [];

    [ObservableProperty]
    List<string> textureClampingType = [];

    [ObservableProperty]
    List<string> textureFormatType = [];

    [ObservableProperty]
    List<string> samplerReductionType = ["Average", "Min", "Max"];

    [ObservableProperty]
    string filtration;

    [ObservableProperty]
    string format = "R8G8B8A8_SRGB";

    [ObservableProperty]
    string clamping;

    [ObservableProperty]
    bool shouldGenerateMips;

    [ObservableProperty]
    bool shouldSupportStorageBinding;

    [ObservableProperty]
    bool shouldKeepCpuBuffers;

    [ObservableProperty]
    string samplerReduction = "Average";

    [ObservableProperty]
    int glbTextureIndex = -1;

    void LoadTextureEnumTypes()
    {
        var engineTypes = MauiProgram.GetService<EngineService>().EngineTypes;

        TextureFiltrationType = engineTypes.Enums["enum Sailor::RHI::ETextureFiltration"];
        TextureClampingType = engineTypes.Enums["enum Sailor::RHI::ETextureClamping"];
        TextureFormatType = engineTypes.Enums["enum Sailor::RHI::EFormat"];
        SamplerReductionType = engineTypes.Enums["enum Sailor::RHI::ESamplerReductionMode"];

        OnPropertyChanged(nameof(Filtration));
        OnPropertyChanged(nameof(Clamping));
        OnPropertyChanged(nameof(Format));
        OnPropertyChanged(nameof(SamplerReduction));
    }

    public override Task PrepareInspectorResources()
    {
        LoadRuntimeDataWithoutDirtyTracking(LoadTextureEnumTypes);
        return Task.CompletedTask;
    }

    public ImageSource Texture { get; private set; } = null;

    public override Task Save() => Save(new TextureFileYamlConverter());

    public override async Task<bool> LoadDependentResources()
    {
        if (!IsLoaded)
        {
            try
            {
                await PrepareInspectorResources();

                LoadRuntimeDataWithoutDirtyTracking(() =>
                {
                    if (GlbTextureIndex != -1)
                    {
                        MemoryStream textureStream = null;
                        GlbExtractor.ExtractTextureFromGLB(Asset.FullName, GlbTextureIndex, out textureStream);
                        Texture = ImageSource.FromStream(() => textureStream);
                    }
                    else
                    {
                        Texture = ImageSource.FromFile(Asset.FullName);
                    }
                });
            }
            catch (Exception ex)
            {
                SetLoadError(ex);
            }

            IsLoaded = Texture != null && !Texture.IsEmpty;
        }

        return true;
    }

    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();

                var yaml = File.ReadAllText(AssetInfo.FullName);
                var deserializer = SerializationUtils.CreateDeserializerBuilder()
                .WithTypeConverter(new TextureFileYamlConverter())
                .Build();

                var intermediateObject = deserializer.Deserialize<TextureFile>(yaml);

                FileId = intermediateObject.FileId ?? new FileId();
                Filename = intermediateObject.Filename ?? new FileId();
                Filtration = EnumValueUtils.Normalize(intermediateObject.Filtration);
                Format = EnumValueUtils.Normalize(intermediateObject.Format);
                Clamping = EnumValueUtils.Normalize(intermediateObject.Clamping);
                ShouldGenerateMips = intermediateObject.ShouldGenerateMips;
                ShouldSupportStorageBinding = intermediateObject.ShouldSupportStorageBinding;
                ShouldKeepCpuBuffers = intermediateObject.ShouldKeepCpuBuffers;
                SamplerReduction = EnumValueUtils.Normalize(intermediateObject.SamplerReduction);
                GlbTextureIndex = intermediateObject.GlbTextureIndex;

                DisplayName = GlbTextureIndex == -1 ? Asset.Name : $"{AssetInfo.Name} (Texture {GlbTextureIndex})";

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

public class TextureFileYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(TextureFile);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .Build();

        var textureFile = new TextureFile();

        // Move to the first mapping entry
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
                        textureFile.FileId = deserializer.Deserialize<FileId>(parser);
                        break;
                    case "filename":
                        textureFile.Filename = deserializer.Deserialize<string>(parser);
                        break;
                    case "filtration":
                        textureFile.Filtration = EnumValueUtils.Normalize(deserializer.Deserialize<string>(parser));
                        break;
                    case "format":
                        textureFile.Format = EnumValueUtils.Normalize(deserializer.Deserialize<string>(parser));
                        break;
                    case "clamping":
                        textureFile.Clamping = EnumValueUtils.Normalize(deserializer.Deserialize<string>(parser));
                        break;
                    case "bShouldGenerateMips":
                        textureFile.ShouldGenerateMips = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bShouldSupportStorageBinding":
                        textureFile.ShouldSupportStorageBinding = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bShouldKeepCpuBuffers":
                        textureFile.ShouldKeepCpuBuffers = deserializer.Deserialize<bool>(parser);
                        break;
                    case "reduction":
                        textureFile.SamplerReduction = EnumValueUtils.Normalize(deserializer.Deserialize<string>(parser));
                        break;
                    case "glbTextureIndex":
                        textureFile.GlbTextureIndex = deserializer.Deserialize<int>(parser);
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

        // Consume the end mapping
        parser.Consume<MappingEnd>();

        return textureFile;
    }

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
        try
        {
            var textureFile = (TextureFile)value;

            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

            // Serialize inherited fields
            emitter.Emit(new Scalar(null, "fileId"));
            emitter.Emit(new Scalar(null, textureFile.FileId?.Value));

            emitter.Emit(new Scalar(null, "filename"));
            emitter.Emit(new Scalar(null, textureFile.Filename));

            // Serialize TextureFile specific fields
            emitter.Emit(new Scalar(null, "filtration"));
            emitter.Emit(new Scalar(null, textureFile.Filtration));

            emitter.Emit(new Scalar(null, "format"));
            emitter.Emit(new Scalar(null, textureFile.Format));

            emitter.Emit(new Scalar(null, "clamping"));
            emitter.Emit(new Scalar(null, textureFile.Clamping));

            emitter.Emit(new Scalar(null, "bShouldGenerateMips"));
            emitter.Emit(new Scalar(null, textureFile.ShouldGenerateMips.ToString().ToLower()));

            emitter.Emit(new Scalar(null, "bShouldSupportStorageBinding"));
            emitter.Emit(new Scalar(null, textureFile.ShouldSupportStorageBinding.ToString().ToLower()));

            emitter.Emit(new Scalar(null, "bShouldKeepCpuBuffers"));
            emitter.Emit(new Scalar(null, textureFile.ShouldKeepCpuBuffers.ToString().ToLower()));

            emitter.Emit(new Scalar(null, "reduction"));
            emitter.Emit(new Scalar(null, textureFile.SamplerReduction));

            emitter.Emit(new Scalar(null, "glbTextureIndex"));
            emitter.Emit(new Scalar(null, textureFile.GlbTextureIndex.ToString().ToLower()));

            emitter.Emit(new MappingEnd());
        }
        catch (Exception ex)
        {
            Console.WriteLine(ex.Message);
        }
    }
}
