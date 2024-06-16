
using SailorEngine;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;

namespace SailorEditor.ViewModels
{
    public class ShaderLibraryFile : AssetFile
    {
        public string Code { get; set; }

        public override async Task<bool> LoadDependentResources()
        {
            if (IsLoaded)
                return true;

            Code = File.ReadAllText(Asset.FullName);

            IsLoaded = true;
            return true;
        }
    }

    public class ShaderLibraryFileYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(ShaderLibraryFile);

        public object ReadYaml(IParser parser, Type type)
        {
            var deserializer = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new FileIdYamlConverter())
                .IgnoreUnmatchedProperties()
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
            var serializer = new SerializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new FileIdYamlConverter())
                .Build();

            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

            emitter.Emit(new Scalar(null, "fileId"));
            emitter.Emit(new Scalar(null, assetFile.FileId));

            emitter.Emit(new Scalar(null, "filename"));
            emitter.Emit(new Scalar(null, assetFile.Filename));

            emitter.Emit(new MappingEnd());
        }
    }
}