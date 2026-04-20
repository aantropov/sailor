using SailorEngine;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using SailorEditor.Utility;

namespace SailorEditor.ViewModels;

public class ShaderFile : AssetFile
{
    string includes;
    string defines;
    string glslCommonShader;
    string glslVertexShader;
    string glslFragmentShader;
    string glslComputeShader;

    public string Includes
    {
        get => includes;
        set => SetProperty(ref includes, value);
    }

    public string Defines
    {
        get => defines;
        set => SetProperty(ref defines, value);
    }

    public string GlslCommonShader
    {
        get => glslCommonShader;
        set => SetProperty(ref glslCommonShader, value);
    }

    public string GlslVertexShader
    {
        get => glslVertexShader;
        set => SetProperty(ref glslVertexShader, value);
    }

    public string GlslFragmentShader
    {
        get => glslFragmentShader;
        set => SetProperty(ref glslFragmentShader, value);
    }

    public string GlslComputeShader
    {
        get => glslComputeShader;
        set => SetProperty(ref glslComputeShader, value);
    }

    public override Task<bool> LoadDependentResources()
    {
        if (IsLoaded)
            return Task.FromResult(true);

        try
        {
            LoadRuntimeDataWithoutDirtyTracking(() =>
            {
                Includes = string.Empty;
                Defines = string.Empty;
                GlslCommonShader = string.Empty;
                GlslVertexShader = string.Empty;
                GlslFragmentShader = string.Empty;
                GlslComputeShader = string.Empty;

                using var yamlAssetInfo = new FileStream(Asset.FullName, FileMode.Open);
                using var reader = new StreamReader(yamlAssetInfo);
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
            });
        }
        catch (Exception e)
        {
            SetLoadError(e);
            return Task.FromResult(false);
        }

        IsLoaded = true;

        return Task.FromResult(true);
    }
}

public class ShaderFileYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(ShaderFile);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
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

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        emitter.Emit(new Scalar(null, "fileId"));
        emitter.Emit(new Scalar(null, assetFile.FileId));

        emitter.Emit(new Scalar(null, "filename"));
        emitter.Emit(new Scalar(null, assetFile.Filename));

        emitter.Emit(new MappingEnd());
    }
}
