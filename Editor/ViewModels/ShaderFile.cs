using SailorEngine;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.ViewModels
{
    public class ShaderFile : AssetFile
    {
        public string Includes { get; set; }

        public string Defines { get; set; }

        public string GlslCommonShader { get; set; }

        public string GlslVertexShader { get; set; }

        public string GlslFragmentShader { get; set; }

        public string GlslComputeShader { get; set; }

        public override async Task<bool> LoadDependentResources()
        {
            if (IsLoaded)
                return true;

            try
            {
                using (var yamlAssetInfo = new FileStream(Asset.FullName, FileMode.Open))
                using (var reader = new StreamReader(yamlAssetInfo))
                {
                    var yaml = new YamlStream();
                    yaml.Load(reader);

                    var root = (YamlMappingNode)yaml.Documents[0].RootNode;

                    foreach (var e in root.Children)
                    {
                        switch (e.Key.ToString())
                        {
                            case "includes":
                                Includes += e.Value.ToString();
                                break;
                            case "defines":
                                Defines += e.Value.ToString();
                                break;
                            case "glslVertex":
                                GlslVertexShader = e.Value.ToString();
                                break;
                            case "glslFragment":
                                GlslFragmentShader = e.Value.ToString();
                                break;
                            case "glslCommon":
                                GlslCommonShader = e.Value.ToString();
                                break;
                            case "glslCompute":
                                GlslComputeShader = e.Value.ToString();
                                break;
                        }
                    }

                    yamlAssetInfo.Close();
                }
            }
            catch (Exception e)
            {
                DisplayName = e.ToString();
                return false;
            }

            IsLoaded = true;

            return true;
        }
    }

    public class ShaderFileYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(ShaderFile);

        public object ReadYaml(IParser parser, Type type)
        {
            var deserializer = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new FileIdYamlConverter())
                .IgnoreUnmatchedProperties()
                .Build();

            var assetFile = new ShaderFile();

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
            var assetFile = (ShaderFile)value;
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