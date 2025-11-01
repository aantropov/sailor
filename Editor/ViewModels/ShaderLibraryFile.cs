
using SailorEngine;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Utility;
using CommunityToolkit.Mvvm.ComponentModel;
using System.IO;
using System.Threading.Tasks;

namespace SailorEditor.ViewModels;

public partial class ShaderLibraryFile : AssetFile
{
    public override async Task<bool> LoadDependentResources()
    {
        if (IsLoaded)
            return true;

        Code = await File.ReadAllTextAsync(Asset.FullName);

        IsLoaded = true;
        return true;
    }

    public string Code { get; set; }
}

public class ShaderLibraryFileYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(ShaderLibraryFile);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new FileIdYamlConverter())
            .Build();

        var assetFile = new ShaderLibraryFile();

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
        var assetFile = (ShaderLibraryFile)value;

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        emitter.Emit(new Scalar(null, "fileId"));
        emitter.Emit(new Scalar(null, assetFile.FileId));

        emitter.Emit(new Scalar(null, "filename"));
        emitter.Emit(new Scalar(null, assetFile.Filename));

        emitter.Emit(new MappingEnd());
    }
}