using System.ComponentModel;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Services;
using System.Numerics;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEngine;
using SailorEditor.Utility;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using YamlDotNet.Core.Tokens;
using Scalar = YamlDotNet.Core.Events.Scalar;
using System;
using System.Windows.Input;
using CommunityToolkit.Mvvm.Input;

namespace SailorEditor.ViewModels;

public partial class MaterialFile : AssetFile
{
    public MaterialFile()
    {
        AddSamplerCommand = new Command(OnAddSampler);
        RemoveSamplerCommand = new Command<UniformFileId>(OnRemoveSampler);
        ClearSamplersCommand = new Command(OnClearSamplers);
        AddUniformVec4Command = new Command(OnAddUniformVec4);
        RemoveUniformVec4Command = new Command<UniformVec4>(OnRemoveUniformVec4);
        ClearUniformsVec4Command = new Command(OnClearUniformsVec4);
        AddUniformFloatCommand = new Command(OnAddUniformFloat);
        RemoveUniformFloatCommand = new Command<UniformFloat>(OnRemoveUniformFloat);
        ClearUniformsFloatCommand = new Command(OnClearUniformsFloat);
        AddShaderDefineCommand = new Command(OnAddShaderDefine);
        RemoveShaderDefineCommand = new Command<Observable<string>>(OnRemoveShaderDefine);
        ClearShaderDefinesCommand = new Command(OnClearShaderDefines);
        SelectSamplerCommand = new Command<string>(OnSelectSampler);
        ClearSamplerCommand = new AsyncRelayCommand<string>(OnClearSampler);
        ClearShaderCommand = new AsyncRelayCommand(OnClearShader);
    }

    [ObservableProperty]
    string renderQueue = "Opaque";

    [ObservableProperty]
    float depthBias = 0.0f;

    [ObservableProperty]
    bool supportMultisampling = true;

    [ObservableProperty]
    bool customDepthShader = true;

    [ObservableProperty]
    bool enableDepthTest = true;

    [ObservableProperty]
    bool enableZWrite = true;

    [ObservableProperty]
    string cullMode;

    [ObservableProperty]
    string blendMode;

    [ObservableProperty]
    string fillMode;

    [ObservableProperty]
    FileId shader;

    [ObservableProperty]
    ObservableList<UniformFileId> samplers = [];

    [ObservableProperty]
    ObservableList<UniformVec4> uniformsVec4 = [];

    [ObservableProperty]
    ObservableList<UniformFloat> uniformsFloat = [];

    [ObservableProperty]
    ObservableList<Observable<string>> shaderDefines = [];

    [ObservableProperty]
    List<string> fillModeEnumValues = [];

    [ObservableProperty]
    List<string> cullModeEnumValues = [];

    [ObservableProperty]
    List<string> blendModeEnumValues = [];

    void LoadMaterialEnumTypes()
    {
        var engineTypes = MauiProgram.GetService<EngineService>().EngineTypes;

        FillModeEnumValues = engineTypes.Enums["enum Sailor::RHI::EFillMode"];
        CullModeEnumValues = engineTypes.Enums["enum Sailor::RHI::ECullMode"];
        BlendModeEnumValues = engineTypes.Enums["enum Sailor::RHI::EBlendMode"];
    }

    public override Task PrepareInspectorResources()
    {
        LoadRuntimeDataWithoutDirtyTracking(LoadMaterialEnumTypes);
        return Task.CompletedTask;
    }

    public override async Task<bool> LoadDependentResources()
    {
        if (!IsLoaded)
        {
            try
            {
                await PrepareInspectorResources();

                var AssetService = MauiProgram.GetService<AssetsService>();
                var preloadTasks = new List<Task>();
                foreach (var tex in Samplers)
                {
                    var task = Task.Run(() =>
                    {
                        if (AssetService.Assets.TryGetValue(tex.Value.Value, out var file))
                        {
                            _ = file.LoadDependentResources();
                        }
                    });

                    preloadTasks.Add(task);
                }

                await Task.WhenAll(preloadTasks);
            }
            catch (Exception ex)
            {
                SetLoadError(ex);
            }

            IsLoaded = true;
        }

        return true;
    }

    public override Task Save()
    {
        EnsureWritable();

        using (var yamlAsset = new FileStream(Asset.FullName, FileMode.Create))
        using (var writer = new StreamWriter(yamlAsset))
        {
            writer.Write(CreatePreviewSnapshot());
        }

        IsDirty = false;
        return Task.CompletedTask;
    }

    public string CreatePreviewSnapshot() => SerializationUtils.CreateSerializerBuilder()
        .WithTypeConverter(new MaterialFileYamlConverter())
        .Build()
        .Serialize(this);

    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();

                // Asset Info
                var yamlAssetInfo = File.ReadAllText(AssetInfo.FullName);

                var deserializerAssetInfo = SerializationUtils.CreateDeserializerBuilder()
                .WithTypeConverter(new AssetFileYamlConverter())
                .Build();

                var intermediateObjectAssetInfo = deserializerAssetInfo.Deserialize<AssetFile>(yamlAssetInfo);
                FileId = intermediateObjectAssetInfo.FileId ?? new FileId();
                Filename = intermediateObjectAssetInfo.Filename ?? new FileId();

                // Asset
                var yamlAsset = File.ReadAllText(Asset.FullName);

                var deserializer = SerializationUtils.CreateDeserializerBuilder()
                .WithTypeConverter(new MaterialFileYamlConverter())
                .Build();

                var intermediateObject = deserializer.Deserialize<MaterialFile>(yamlAsset);

                RenderQueue = intermediateObject.RenderQueue;
                DepthBias = intermediateObject.DepthBias;
                SupportMultisampling = intermediateObject.SupportMultisampling;
                CustomDepthShader = intermediateObject.CustomDepthShader;
                EnableDepthTest = intermediateObject.EnableDepthTest;
                EnableZWrite = intermediateObject.EnableZWrite;
                CullMode = EnumValueUtils.Normalize(intermediateObject.CullMode);
                BlendMode = EnumValueUtils.Normalize(intermediateObject.BlendMode);
                FillMode = EnumValueUtils.Normalize(intermediateObject.FillMode);
                Shader = intermediateObject.Shader;
                Samplers = intermediateObject.Samplers;
                UniformsVec4 = intermediateObject.UniformsVec4;
                UniformsFloat = intermediateObject.UniformsFloat;
                ShaderDefines = intermediateObject.ShaderDefines;

                DisplayName = Asset.Name;

                ShaderDefines.CollectionChanged += (a, e) => MarkDirty(nameof(ShaderDefines));
                ShaderDefines.ItemChanged += (a, e) => MarkDirty(nameof(ShaderDefines));

                UniformsFloat.CollectionChanged += (a, e) => MarkDirty(nameof(UniformsFloat));
                UniformsFloat.ItemChanged += (a, e) => MarkDirty(nameof(UniformsFloat));

                UniformsVec4.CollectionChanged += (a, e) => MarkDirty(nameof(UniformsVec4));
                UniformsVec4.ItemChanged += (a, e) => MarkDirty(nameof(UniformsVec4));

                Samplers.CollectionChanged += (a, e) => MarkDirty(nameof(Samplers));
                Samplers.ItemChanged += (a, e) => MarkDirty(nameof(Samplers));

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

    public ICommand AddSamplerCommand { get; }
    public ICommand RemoveSamplerCommand { get; }
    public ICommand ClearSamplersCommand { get; }
    public ICommand AddUniformVec4Command { get; }
    public ICommand RemoveUniformVec4Command { get; }
    public ICommand ClearUniformsVec4Command { get; }
    public ICommand AddUniformFloatCommand { get; }
    public ICommand RemoveUniformFloatCommand { get; }
    public ICommand ClearUniformsFloatCommand { get; }
    public ICommand AddShaderDefineCommand { get; }
    public ICommand RemoveShaderDefineCommand { get; }
    public ICommand ClearShaderDefinesCommand { get; }
    public ICommand SelectSamplerCommand { get; }
    public IAsyncRelayCommand ClearSamplerCommand { get; }
    public IAsyncRelayCommand ClearShaderCommand { get; }

    private void OnAddSampler() => Samplers.Add(new UniformFileId { Key = "material.newSampler" });
    private void OnRemoveSampler(UniformFileId sampler) => Samplers.Remove(sampler);
    private void OnClearSamplers() => Samplers.Clear();
    private void OnAddUniformVec4() => UniformsVec4.Add(new UniformVec4 { Key = "material.newVec4Variable" });
    private void OnRemoveUniformVec4(UniformVec4 uniform) => UniformsVec4.Remove(uniform);
    private void OnClearUniformsVec4() => UniformsVec4.Clear();
    private void OnAddUniformFloat() => UniformsFloat.Add(new UniformFloat { Key = "material.newFloatVariable" });
    private void OnRemoveUniformFloat(UniformFloat uniform) => UniformsFloat.Remove(uniform);
    private void OnClearUniformsFloat() => UniformsFloat.Clear();
    private void OnAddShaderDefine()
    {
        ShaderDefines.Add(new Observable<string>("New Define"));
        OnPropertyChanged(nameof(ShaderDefines));
    }

    private void OnRemoveShaderDefine(Observable<string> shaderDefine)
    {
        ShaderDefines.Remove(shaderDefine);
        OnPropertyChanged(nameof(ShaderDefines));
    }

    private void OnClearShaderDefines()
    {
        ShaderDefines.Clear();
        OnPropertyChanged(nameof(ShaderDefines));
    }
    private void OnSelectSampler(string key)
    {
        var assetService = MauiProgram.GetService<AssetsService>();

        foreach (var uniform in Samplers)
        {
            if (uniform.Key == key)
            {
                var asset = assetService.Assets[uniform.Value];
                if (asset != null)
                {
                    var selecitonService = MauiProgram.GetService<SelectionService>();
                    selecitonService.SelectObject(asset);
                }
                break;
            }
        }
    }

    private Task OnClearSampler(string key)
    {
        foreach (var uniform in Samplers)
        {
            if (uniform.Key == key)
            {
                uniform.Value = default;
                break;
            }
        }

        return Task.CompletedTask;
    }

    private Task OnClearShader()
    {
        Shader = FileId.NullFileId;
        return Task.CompletedTask;
    }
}

public class MaterialFileYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(MaterialFile);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new ObservableListConverter<Observable<FileId>>([new ObservableObjectYamlConverter<FileId>()]))
            .WithTypeConverter(new ObservableListConverter<Uniform<FileId>>([new UniformYamlConverter<FileId>()]))
            .WithTypeConverter(new ObservableListConverter<Uniform<Vec4>>([new UniformYamlConverter<Vec4>()]))
            .WithTypeConverter(new ObservableListConverter<Uniform<float>>([new UniformYamlConverter<float>()]))
            .WithTypeConverter(new ObservableListConverter<Observable<string>>([new ObservableObjectYamlConverter<string>()]))
            .Build();

        var assetFile = new MaterialFile();

        parser.Consume<MappingStart>();

        while (parser.Current is not MappingEnd)
        {
            if (parser.Current is Scalar scalar)
            {
                var propertyName = scalar.Value;
                parser.MoveNext(); // Move to the value

                switch (propertyName)
                {
                    case "renderQueue":
                        assetFile.RenderQueue = deserializer.Deserialize<string>(parser);
                        break;
                    case "depthBias":
                        assetFile.DepthBias = deserializer.Deserialize<float>(parser);
                        break;
                    case "bSupportMultisampling":
                        assetFile.SupportMultisampling = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bCustomDepthShader":
                        assetFile.CustomDepthShader = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bEnableDepthTest":
                        assetFile.EnableDepthTest = deserializer.Deserialize<bool>(parser);
                        break;
                    case "bEnableZWrite":
                        assetFile.EnableZWrite = deserializer.Deserialize<bool>(parser);
                        break;
                    case "cullMode":
                        assetFile.CullMode = EnumValueUtils.Normalize(deserializer.Deserialize<string>(parser));
                        break;
                    case "blendMode":
                        assetFile.BlendMode = EnumValueUtils.Normalize(deserializer.Deserialize<string>(parser));
                        break;
                    case "fillMode":
                        assetFile.FillMode = EnumValueUtils.Normalize(deserializer.Deserialize<string>(parser));
                        break;
                    case "shaderUid":
                        assetFile.Shader = deserializer.Deserialize<FileId>(parser);
                        break;
                    case "samplers":
                        var samplers = deserializer.Deserialize<Dictionary<string, FileId>>(parser) ?? [];
                        assetFile.Samplers = new ObservableList<UniformFileId>(samplers.Select(s => new UniformFileId { Key = s.Key, Value = s.Value }).ToList());
                        break;
                    case "uniformsVec4":
                        var uniformsVec4 = deserializer.Deserialize<Dictionary<string, Vec4>>(parser) ?? [];
                        assetFile.UniformsVec4 = new ObservableList<UniformVec4>(uniformsVec4.Select(s => new UniformVec4 { Key = s.Key, Value = s.Value }).ToList());
                        break;
                    case "uniformsFloat":
                        var uniformsFloat = deserializer.Deserialize<Dictionary<string, float>>(parser) ?? [];
                        assetFile.UniformsFloat = new ObservableList<UniformFloat>(uniformsFloat.Select(s => new UniformFloat { Key = s.Key, Value = s.Value }).ToList());
                        break;
                    case "defines":
                        assetFile.ShaderDefines = deserializer.Deserialize<ObservableList<Observable<string>>>(parser);
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
        var assetFile = (MaterialFile)value;

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        // Serialize MaterialFile specific fields
        emitter.Emit(new Scalar(null, "renderQueue"));
        emitter.Emit(new Scalar(null, assetFile.RenderQueue));

        emitter.Emit(new Scalar(null, "depthBias"));
        emitter.Emit(new Scalar(null, assetFile.DepthBias.ToString(CultureInfo.InvariantCulture)));

        emitter.Emit(new Scalar(null, "bSupportMultisampling"));
        emitter.Emit(new Scalar(null, assetFile.SupportMultisampling.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "bCustomDepthShader"));
        emitter.Emit(new Scalar(null, assetFile.CustomDepthShader.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "bEnableDepthTest"));
        emitter.Emit(new Scalar(null, assetFile.EnableDepthTest.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "bEnableZWrite"));
        emitter.Emit(new Scalar(null, assetFile.EnableZWrite.ToString().ToLower()));

        emitter.Emit(new Scalar(null, "cullMode"));
        emitter.Emit(new Scalar(null, assetFile.CullMode));

        emitter.Emit(new Scalar(null, "blendMode"));
        emitter.Emit(new Scalar(null, assetFile.BlendMode));

        emitter.Emit(new Scalar(null, "fillMode"));
        emitter.Emit(new Scalar(null, assetFile.FillMode));

        emitter.Emit(new Scalar(null, "shaderUid"));
        emitter.Emit(new Scalar(null, assetFile.Shader?.Value));

        emitter.Emit(new Scalar(null, "samplers"));
        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));
        foreach (var sampler in assetFile.Samplers)
        {
            emitter.Emit(new Scalar(null, sampler.Key));
            emitter.Emit(new Scalar(null, sampler.Value?.Value));
        }
        emitter.Emit(new MappingEnd());

        emitter.Emit(new Scalar(null, "uniformsVec4"));
        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));
        foreach (var uniform in assetFile.UniformsVec4)
        {
            emitter.Emit(new Scalar(null, uniform.Key));
            emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
            emitter.Emit(new Scalar(null, uniform.Value.X.ToString(CultureInfo.InvariantCulture)));
            emitter.Emit(new Scalar(null, uniform.Value.Y.ToString(CultureInfo.InvariantCulture)));
            emitter.Emit(new Scalar(null, uniform.Value.Z.ToString(CultureInfo.InvariantCulture)));
            emitter.Emit(new Scalar(null, uniform.Value.W.ToString(CultureInfo.InvariantCulture)));
            emitter.Emit(new SequenceEnd());
        }
        emitter.Emit(new MappingEnd());

        emitter.Emit(new Scalar(null, "uniformsFloat"));
        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));
        foreach (var uniform in assetFile.UniformsFloat)
        {
            emitter.Emit(new Scalar(null, uniform.Key));
            emitter.Emit(new Scalar(null, uniform.Value.ToString(CultureInfo.InvariantCulture)));
        }
        emitter.Emit(new MappingEnd());

        emitter.Emit(new Scalar(null, "defines"));
        emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
        foreach (var define in assetFile.ShaderDefines)
        {
            emitter.Emit(new Scalar(null, define.Value));
        }
        emitter.Emit(new SequenceEnd());

        emitter.Emit(new MappingEnd());
    }
}
