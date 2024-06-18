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

    public List<string> TextureFiltrationType { get; set; } = [];
    public List<string> TextureClampingType { get; set; } = [];
    public List<string> TextureFormatType { get; set; } = [];

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

    public ImageSource Texture { get; private set; } = null;

    public override async Task Save() => await Save(new TextureFileYamlConverter());

    public override async Task<bool> LoadDependentResources()
    {
        if (!IsLoaded)
        {
            try
            {
                var engineTypes = MauiProgram.GetService<EngineService>().EngineTypes;

                TextureFiltrationType = engineTypes.Enums["enum Sailor::RHI::ETextureFiltration"];
                TextureClampingType = engineTypes.Enums["enum Sailor::RHI::ETextureClamping"];
                TextureFormatType = engineTypes.Enums["enum Sailor::RHI::EFormat"];

                Texture = ImageSource.FromFile(Asset.FullName);
            }
            catch (Exception ex)
            {
                DisplayName = ex.Message;
            }

            IsLoaded = Texture != null && !Texture.IsEmpty;
        }

        return true;
    }

    public override async Task Revert()
    {
        try
        {
            var yaml = File.ReadAllText(AssetInfo.FullName);
            var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new TextureFileYamlConverter())
            .IgnoreUnmatchedProperties()
            .Build();

            var intermediateObject = deserializer.Deserialize<TextureFile>(yaml);

            FileId = intermediateObject.FileId;
            Filename = intermediateObject.Filename;
            Filtration = intermediateObject.Filtration;
            Format = intermediateObject.Format;
            Clamping = intermediateObject.Clamping;
            ShouldGenerateMips = intermediateObject.ShouldGenerateMips;
            ShouldSupportStorageBinding = intermediateObject.ShouldSupportStorageBinding;

            DisplayName = Asset.Name;

            IsLoaded = false;
        }
        catch (Exception ex)
        {
            DisplayName = ex.Message;
        }

        IsDirty = false;
    }
}

public class TextureFileYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(TextureFile);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .WithTypeConverter(new FileIdYamlConverter())
            .IgnoreUnmatchedProperties()
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
                        textureFile.Filtration = deserializer.Deserialize<string>(parser);
                        break;
                    case "format":
                        textureFile.Format = deserializer.Deserialize<string>(parser);
                        break;
                    case "clamping":
                        textureFile.Clamping = deserializer.Deserialize<string>(parser);
                        break;
                    case "bShouldGenerateMips":
                        textureFile.ShouldGenerateMips = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bShouldSupportStorageBinding":
                        textureFile.ShouldSupportStorageBinding = deserializer.Deserialize<bool>(parser);
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

            emitter.Emit(new MappingEnd());
        }
        catch (Exception ex)
        {
            Console.WriteLine(ex.Message);
        }
    }
}