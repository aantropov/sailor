using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Globalization;
using System.Windows.Input;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.ViewModels;

public partial class FrameGraphFile : AssetFile
{
    [ObservableProperty]
    ObservableList<FrameGraphSampler> samplers = [];

    [ObservableProperty]
    ObservableList<FrameGraphScalar> floats = [];

    [ObservableProperty]
    ObservableList<FrameGraphRenderTarget> renderTargets = [];

    [ObservableProperty]
    ObservableList<FrameGraphNode> nodes = [];

    public FrameGraphFile()
    {
        AddSamplerCommand = new Command(() => Samplers.Add(new FrameGraphSampler()));
        RemoveSamplerCommand = new Command<FrameGraphSampler>(sampler => Samplers.Remove(sampler));
        ClearSamplersCommand = new Command(() => Samplers.Clear());

        AddFloatCommand = new Command(() => Floats.Add(new FrameGraphScalar()));
        RemoveFloatCommand = new Command<FrameGraphScalar>(value => Floats.Remove(value));
        ClearFloatsCommand = new Command(() => Floats.Clear());

        AddRenderTargetCommand = new Command(() => RenderTargets.Add(new FrameGraphRenderTarget()));
        RemoveRenderTargetCommand = new Command<FrameGraphRenderTarget>(target => RenderTargets.Remove(target));
        ClearRenderTargetsCommand = new Command(() => RenderTargets.Clear());

        AddNodeCommand = new Command(() => Nodes.Add(new FrameGraphNode()));
        RemoveNodeCommand = new Command<FrameGraphNode>(node => Nodes.Remove(node));
        ClearNodesCommand = new Command(() => Nodes.Clear());
    }

    public ICommand AddSamplerCommand { get; }
    public ICommand RemoveSamplerCommand { get; }
    public ICommand ClearSamplersCommand { get; }
    public ICommand AddFloatCommand { get; }
    public ICommand RemoveFloatCommand { get; }
    public ICommand ClearFloatsCommand { get; }
    public ICommand AddRenderTargetCommand { get; }
    public ICommand RemoveRenderTargetCommand { get; }
    public ICommand ClearRenderTargetsCommand { get; }
    public ICommand AddNodeCommand { get; }
    public ICommand RemoveNodeCommand { get; }
    public ICommand ClearNodesCommand { get; }

    public override Task Save()
    {
        SaveRendererAsset();
        return base.Save();
    }

    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();
                LoadRendererAsset();
                DisplayName = Asset.Name;
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

    void LoadRendererAsset()
    {
        var stream = new YamlStream();
        using (var reader = new StreamReader(Asset.FullName))
        {
            stream.Load(reader);
        }

        var root = stream.Documents.Count > 0 ? stream.Documents[0].RootNode as YamlMappingNode : null;
        Samplers = ReadList(root, "samplers", FrameGraphSampler.FromYaml);
        Floats = ReadNamedScalarList(root, "float");
        RenderTargets = ReadList(root, "renderTargets", FrameGraphRenderTarget.FromYaml);
        Nodes = ReadList(root, "frame", FrameGraphNode.FromYaml);

        TrackList(Samplers, nameof(Samplers));
        TrackList(Floats, nameof(Floats));
        TrackList(RenderTargets, nameof(RenderTargets));
        TrackList(Nodes, nameof(Nodes));
    }

    void SaveRendererAsset()
    {
        var root = new YamlMappingNode
        {
            { "samplers", WriteList(Samplers, sampler => sampler.ToYaml()) },
            { "float", WriteNamedScalarList(Floats) },
            { "renderTargets", WriteList(RenderTargets, target => target.ToYaml()) },
            { "frame", WriteList(Nodes, node => node.ToYaml()) }
        };

        var yaml = new YamlStream(new YamlDocument(root));
        using var writer = new StreamWriter(new FileStream(Asset.FullName, FileMode.Create));
        yaml.Save(writer, false);
    }

    void TrackList<T>(ObservableList<T> list, string propertyName)
        where T : INotifyPropertyChanged
    {
        list.CollectionChanged += (_, _) => MarkDirty(propertyName);
        list.ItemChanged += (_, _) => MarkDirty(propertyName);
    }

    static ObservableList<T> ReadList<T>(YamlMappingNode root, string name, Func<YamlMappingNode, T> factory)
        where T : INotifyPropertyChanged
    {
        var result = new ObservableList<T>();
        if (root?.Children.TryGetValue(new YamlScalarNode(name), out var node) != true ||
            node is not YamlSequenceNode sequence)
        {
            return result;
        }

        foreach (var item in sequence.Children.OfType<YamlMappingNode>())
        {
            result.Add(factory(item));
        }

        return result;
    }

    static ObservableList<FrameGraphScalar> ReadNamedScalarList(YamlMappingNode root, string name)
    {
        var result = new ObservableList<FrameGraphScalar>();
        if (root?.Children.TryGetValue(new YamlScalarNode(name), out var node) != true ||
            node is not YamlSequenceNode sequence)
        {
            return result;
        }

        foreach (var item in sequence.Children.OfType<YamlMappingNode>())
        {
            foreach (var entry in item.Children)
            {
                result.Add(new FrameGraphScalar
                {
                    Key = ScalarToString(entry.Key),
                    Value = ScalarToString(entry.Value)
                });
            }
        }

        return result;
    }

    static YamlSequenceNode WriteList<T>(IEnumerable<T> values, Func<T, YamlMappingNode> serialize)
    {
        var sequence = new YamlSequenceNode();
        foreach (var value in values)
        {
            sequence.Add(serialize(value));
        }
        return sequence;
    }

    static YamlSequenceNode WriteNamedScalarList(IEnumerable<FrameGraphScalar> values)
    {
        var sequence = new YamlSequenceNode();
        foreach (var value in values)
        {
            sequence.Add(new YamlMappingNode { { value.Key ?? string.Empty, Scalar(value.Value) } });
        }
        return sequence;
    }

    static string ScalarToString(YamlNode node) => node switch
    {
        YamlScalarNode scalar => scalar.Value ?? "~",
        _ => node?.ToString() ?? string.Empty
    };

    static YamlScalarNode Scalar(string value) => value == "~" ? new YamlScalarNode(null) : new YamlScalarNode(value ?? string.Empty);
}

public partial class FrameGraphSampler : ObservableObject
{
    [ObservableProperty]
    string name = string.Empty;

    [ObservableProperty]
    string fileId = string.Empty;

    [ObservableProperty]
    string path = string.Empty;

    public static FrameGraphSampler FromYaml(YamlMappingNode node) => new()
    {
        Name = ReadString(node, "name"),
        FileId = ReadString(node, "fileId"),
        Path = ReadString(node, "path")
    };

    public YamlMappingNode ToYaml() => new()
    {
        { "name", Name ?? string.Empty },
        { "fileId", FileId ?? string.Empty },
        { "path", Path ?? string.Empty }
    };

    static string ReadString(YamlMappingNode node, string name) =>
        node.Children.TryGetValue(new YamlScalarNode(name), out var value) && value is YamlScalarNode scalar ? scalar.Value ?? string.Empty : string.Empty;
}

public partial class FrameGraphRenderTarget : ObservableObject
{
    [ObservableProperty]
    string name = string.Empty;

    [ObservableProperty]
    string format = "R16G16B16A16_SFLOAT";

    [ObservableProperty]
    string width = "ViewportWidth";

    [ObservableProperty]
    string height = "ViewportHeight";

    [ObservableProperty]
    string clamping = string.Empty;

    [ObservableProperty]
    string filtration = string.Empty;

    [ObservableProperty]
    string reduction = string.Empty;

    [ObservableProperty]
    bool isSurface;

    [ObservableProperty]
    bool isCompatibleWithComputeShaders;

    [ObservableProperty]
    bool generateMips;

    [ObservableProperty]
    int maxMipLevel;

    public static FrameGraphRenderTarget FromYaml(YamlMappingNode node) => new()
    {
        Name = ReadString(node, "name"),
        Format = ReadString(node, "format", "R16G16B16A16_SFLOAT"),
        Width = ReadString(node, "width", "ViewportWidth"),
        Height = ReadString(node, "height", "ViewportHeight"),
        Clamping = ReadString(node, "clamping"),
        Filtration = ReadString(node, "filtration"),
        Reduction = ReadString(node, "reduction"),
        IsSurface = ReadBool(node, "bIsSurface"),
        IsCompatibleWithComputeShaders = ReadBool(node, "bIsCompatibleWithComputeShaders"),
        GenerateMips = ReadBool(node, "bGenerateMips"),
        MaxMipLevel = ReadInt(node, "maxMipLevel")
    };

    public YamlMappingNode ToYaml()
    {
        var node = new YamlMappingNode
        {
            { "name", Name ?? string.Empty },
            { "format", Format ?? string.Empty },
            { "width", Width ?? string.Empty },
            { "height", Height ?? string.Empty }
        };

        AddIfNotEmpty(node, "clamping", Clamping);
        AddIfNotEmpty(node, "filtration", Filtration);
        AddIfNotEmpty(node, "reduction", Reduction);
        node.Add("bIsSurface", IsSurface.ToString().ToLowerInvariant());
        node.Add("bIsCompatibleWithComputeShaders", IsCompatibleWithComputeShaders.ToString().ToLowerInvariant());
        node.Add("bGenerateMips", GenerateMips.ToString().ToLowerInvariant());
        if (MaxMipLevel != 0)
        {
            node.Add("maxMipLevel", MaxMipLevel.ToString(CultureInfo.InvariantCulture));
        }

        return node;
    }

    static void AddIfNotEmpty(YamlMappingNode node, string key, string value)
    {
        if (!string.IsNullOrWhiteSpace(value))
        {
            node.Add(key, value);
        }
    }

    static string ReadString(YamlMappingNode node, string name, string fallback = "") =>
        node.Children.TryGetValue(new YamlScalarNode(name), out var value) && value is YamlScalarNode scalar ? scalar.Value ?? fallback : fallback;

    static bool ReadBool(YamlMappingNode node, string name) =>
        node.Children.TryGetValue(new YamlScalarNode(name), out var value) && bool.TryParse((value as YamlScalarNode)?.Value, out var parsed) && parsed;

    static int ReadInt(YamlMappingNode node, string name) =>
        node.Children.TryGetValue(new YamlScalarNode(name), out var value) && int.TryParse((value as YamlScalarNode)?.Value, out var parsed) ? parsed : 0;
}

public partial class FrameGraphScalar : ObservableObject
{
    [ObservableProperty]
    string key = string.Empty;

    [ObservableProperty]
    string value = string.Empty;
}

public partial class FrameGraphVec4 : ObservableObject
{
    [ObservableProperty]
    float x;

    [ObservableProperty]
    float y;

    [ObservableProperty]
    float z;

    [ObservableProperty]
    float w;

    public static FrameGraphVec4 FromYaml(YamlNode node)
    {
        var values = node is YamlSequenceNode sequence
            ? sequence.Children.OfType<YamlScalarNode>().Select(x => float.TryParse(x.Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) ? parsed : 0.0f).ToList()
            : [];

        return new FrameGraphVec4
        {
            X = values.ElementAtOrDefault(0),
            Y = values.ElementAtOrDefault(1),
            Z = values.ElementAtOrDefault(2),
            W = values.ElementAtOrDefault(3)
        };
    }

    public YamlSequenceNode ToYaml() => new()
    {
        X.ToString(CultureInfo.InvariantCulture),
        Y.ToString(CultureInfo.InvariantCulture),
        Z.ToString(CultureInfo.InvariantCulture),
        W.ToString(CultureInfo.InvariantCulture)
    };
}

public partial class FrameGraphVec4Value : ObservableObject
{
    [ObservableProperty]
    string key = string.Empty;

    [ObservableProperty]
    FrameGraphVec4 value = new();
}

public partial class FrameGraphTargetBinding : ObservableObject
{
    [ObservableProperty]
    string key = "target";

    [ObservableProperty]
    string value = string.Empty;
}

public partial class FrameGraphNode : ObservableObject
{
    [ObservableProperty]
    string name = string.Empty;

    public FrameGraphNode()
    {
        AddStringCommand = new Command(() => Strings.Add(new FrameGraphScalar()));
        RemoveStringCommand = new Command<FrameGraphScalar>(value => Strings.Remove(value));
        ClearStringsCommand = new Command(() => Strings.Clear());

        AddFloatCommand = new Command(() => Floats.Add(new FrameGraphScalar()));
        RemoveFloatCommand = new Command<FrameGraphScalar>(value => Floats.Remove(value));
        ClearFloatsCommand = new Command(() => Floats.Clear());

        AddVec4Command = new Command(() => Vec4.Add(new FrameGraphVec4Value()));
        RemoveVec4Command = new Command<FrameGraphVec4Value>(value => Vec4.Remove(value));
        ClearVec4Command = new Command(() => Vec4.Clear());

        AddRenderTargetCommand = new Command(() => RenderTargets.Add(new FrameGraphTargetBinding()));
        RemoveRenderTargetCommand = new Command<FrameGraphTargetBinding>(value => RenderTargets.Remove(value));
        ClearRenderTargetsCommand = new Command(() => RenderTargets.Clear());

        TrackList(Strings, nameof(Strings));
        TrackList(Floats, nameof(Floats));
        TrackList(Vec4, nameof(Vec4));
        TrackList(RenderTargets, nameof(RenderTargets));
    }

    public ObservableList<FrameGraphScalar> Strings { get; } = [];
    public ObservableList<FrameGraphScalar> Floats { get; } = [];
    public ObservableList<FrameGraphVec4Value> Vec4 { get; } = [];
    public ObservableList<FrameGraphTargetBinding> RenderTargets { get; } = [];

    public ICommand AddStringCommand { get; }
    public ICommand RemoveStringCommand { get; }
    public ICommand ClearStringsCommand { get; }
    public ICommand AddFloatCommand { get; }
    public ICommand RemoveFloatCommand { get; }
    public ICommand ClearFloatsCommand { get; }
    public ICommand AddVec4Command { get; }
    public ICommand RemoveVec4Command { get; }
    public ICommand ClearVec4Command { get; }
    public ICommand AddRenderTargetCommand { get; }
    public ICommand RemoveRenderTargetCommand { get; }
    public ICommand ClearRenderTargetsCommand { get; }

    public static FrameGraphNode FromYaml(YamlMappingNode node)
    {
        var result = new FrameGraphNode { Name = ReadString(node, "name") };
        ReadNamedScalarList(node, "string", result.Strings);
        ReadNamedScalarList(node, "float", result.Floats);
        ReadVec4List(node, result.Vec4);
        ReadRenderTargets(node, result.RenderTargets);
        return result;
    }

    public YamlMappingNode ToYaml()
    {
        var node = new YamlMappingNode { { "name", Name ?? string.Empty } };
        AddNamedScalarList(node, "string", Strings);
        AddNamedScalarList(node, "float", Floats);
        AddVec4List(node, Vec4);
        AddRenderTargets(node, RenderTargets);
        return node;
    }

    void TrackList<T>(ObservableList<T> list, string propertyName)
        where T : INotifyPropertyChanged
    {
        list.CollectionChanged += ListChanged;
        list.ItemChanged += (_, _) => OnPropertyChanged(propertyName);

        void ListChanged(object sender, NotifyCollectionChangedEventArgs args)
        {
            OnPropertyChanged(propertyName);
        }
    }

    static string ReadString(YamlMappingNode node, string name) =>
        node.Children.TryGetValue(new YamlScalarNode(name), out var value) && value is YamlScalarNode scalar ? scalar.Value ?? string.Empty : string.Empty;

    static void ReadNamedScalarList(YamlMappingNode node, string name, ObservableList<FrameGraphScalar> values)
    {
        if (!node.Children.TryGetValue(new YamlScalarNode(name), out var value) ||
            value is not YamlSequenceNode sequence)
        {
            return;
        }

        foreach (var item in sequence.Children.OfType<YamlMappingNode>())
        {
            foreach (var entry in item.Children)
            {
                values.Add(new FrameGraphScalar
                {
                    Key = ScalarToString(entry.Key),
                    Value = ScalarToString(entry.Value)
                });
            }
        }
    }

    static void ReadVec4List(YamlMappingNode node, ObservableList<FrameGraphVec4Value> values)
    {
        if (!node.Children.TryGetValue(new YamlScalarNode("vec4"), out var value) ||
            value is not YamlSequenceNode sequence)
        {
            return;
        }

        foreach (var item in sequence.Children.OfType<YamlMappingNode>())
        {
            foreach (var entry in item.Children)
            {
                values.Add(new FrameGraphVec4Value
                {
                    Key = ScalarToString(entry.Key),
                    Value = FrameGraphVec4.FromYaml(entry.Value)
                });
            }
        }
    }

    static void ReadRenderTargets(YamlMappingNode node, ObservableList<FrameGraphTargetBinding> values)
    {
        if (!node.Children.TryGetValue(new YamlScalarNode("renderTargets"), out var value) ||
            value is not YamlSequenceNode sequence)
        {
            return;
        }

        foreach (var item in sequence.Children.OfType<YamlMappingNode>())
        {
            foreach (var entry in item.Children)
            {
                values.Add(new FrameGraphTargetBinding
                {
                    Key = ScalarToString(entry.Key),
                    Value = ScalarToString(entry.Value)
                });
            }
        }
    }

    static void AddNamedScalarList(YamlMappingNode node, string name, ObservableList<FrameGraphScalar> values)
    {
        if (values.Count == 0)
        {
            return;
        }

        var sequence = new YamlSequenceNode();
        foreach (var value in values)
        {
            sequence.Add(new YamlMappingNode { { value.Key ?? string.Empty, Scalar(value.Value) } });
        }
        node.Add(name, sequence);
    }

    static void AddVec4List(YamlMappingNode node, ObservableList<FrameGraphVec4Value> values)
    {
        if (values.Count == 0)
        {
            return;
        }

        var sequence = new YamlSequenceNode();
        foreach (var value in values)
        {
            sequence.Add(new YamlMappingNode { { value.Key ?? string.Empty, value.Value?.ToYaml() ?? new FrameGraphVec4().ToYaml() } });
        }
        node.Add("vec4", sequence);
    }

    static void AddRenderTargets(YamlMappingNode node, ObservableList<FrameGraphTargetBinding> values)
    {
        if (values.Count == 0)
        {
            return;
        }

        var sequence = new YamlSequenceNode();
        foreach (var value in values)
        {
            sequence.Add(new YamlMappingNode { { value.Key ?? string.Empty, value.Value ?? string.Empty } });
        }
        node.Add("renderTargets", sequence);
    }

    static string ScalarToString(YamlNode node) => node switch
    {
        YamlScalarNode scalar => scalar.Value ?? "~",
        _ => node?.ToString() ?? string.Empty
    };

    static YamlScalarNode Scalar(string value) => value == "~" ? new YamlScalarNode(null) : new YamlScalarNode(value ?? string.Empty);
}
