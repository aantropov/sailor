using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Helpers;
using SailorEditor.Utility;
using SailorEditor.Workflow;
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
    ObservableList<FrameGraphVec4Value> vec4 = [];

    [ObservableProperty]
    ObservableList<FrameGraphRenderTarget> renderTargets = [];

    [ObservableProperty]
    ObservableList<FrameGraphNode> nodes = [];

    [ObservableProperty]
    FrameGraphNode? selectedNode;

    public bool HasSelectedNode => SelectedNode is not null;

    public FrameGraphFile()
    {
        AddSamplerCommand = new Command(() => Samplers.Add(new FrameGraphSampler()));
        RemoveSamplerCommand = new Command<FrameGraphSampler>(sampler => Samplers.Remove(sampler));
        ClearSamplersCommand = new Command(() => Samplers.Clear());

        AddFloatCommand = new Command(() => Floats.Add(new FrameGraphScalar()));
        RemoveFloatCommand = new Command<FrameGraphScalar>(value => Floats.Remove(value));
        ClearFloatsCommand = new Command(() => Floats.Clear());

        AddVec4Command = new Command(() => Vec4.Add(new FrameGraphVec4Value()));
        RemoveVec4Command = new Command<FrameGraphVec4Value>(value => Vec4.Remove(value));
        ClearVec4Command = new Command(() => Vec4.Clear());

        AddRenderTargetCommand = new Command(() => RenderTargets.Add(new FrameGraphRenderTarget()));
        RemoveRenderTargetCommand = new Command<FrameGraphRenderTarget>(target => RenderTargets.Remove(target));
        ClearRenderTargetsCommand = new Command(() => RenderTargets.Clear());

        AddNodeCommand = new Command(() =>
        {
            var node = new FrameGraphNode();
            Nodes.Add(node);
            SelectedNode = node;
        });
        RemoveNodeCommand = new Command<FrameGraphNode>(RemoveNode);
        ClearNodesCommand = new Command(() =>
        {
            Nodes.Clear();
            SelectedNode = null;
        });
        MoveNodeEarlierCommand = new Command<FrameGraphNode>(node => MoveNode(node, -1));
        MoveNodeLaterCommand = new Command<FrameGraphNode>(node => MoveNode(node, 1));
    }

    public ICommand AddSamplerCommand { get; }
    public ICommand RemoveSamplerCommand { get; }
    public ICommand ClearSamplersCommand { get; }
    public ICommand AddFloatCommand { get; }
    public ICommand RemoveFloatCommand { get; }
    public ICommand ClearFloatsCommand { get; }
    public ICommand AddVec4Command { get; }
    public ICommand RemoveVec4Command { get; }
    public ICommand ClearVec4Command { get; }
    public ICommand AddRenderTargetCommand { get; }
    public ICommand RemoveRenderTargetCommand { get; }
    public ICommand ClearRenderTargetsCommand { get; }
    public ICommand AddNodeCommand { get; }
    public ICommand RemoveNodeCommand { get; }
    public ICommand ClearNodesCommand { get; }
    public ICommand MoveNodeEarlierCommand { get; }
    public ICommand MoveNodeLaterCommand { get; }

    partial void OnSelectedNodeChanged(FrameGraphNode? value) =>
        OnPropertyChanged(nameof(HasSelectedNode));

    partial void OnNodesChanged(ObservableList<FrameGraphNode> value)
    {
        if (SelectedNode is not null && !value.Contains(SelectedNode))
        {
            SelectedNode = null;
        }
    }

    void RemoveNode(FrameGraphNode? node)
    {
        if (node is null)
        {
            return;
        }

        var index = Nodes.IndexOf(node);
        if (index < 0)
        {
            return;
        }

        Nodes.RemoveAt(index);
        SelectedNode = Nodes.Count == 0
            ? null
            : Nodes[Math.Min(index, Nodes.Count - 1)];
    }

    void MoveNode(FrameGraphNode? node, int offset)
    {
        if (FrameGraphDiagramOrder.Move(Nodes, node, offset))
        {
            SelectedNode = node;
        }
    }

    public override Task Save()
    {
        EnsureWritable();
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
        Vec4 = ReadNamedVec4List(root, "vec4");
        RenderTargets = ReadList(root, "renderTargets", FrameGraphRenderTarget.FromYaml);
        Nodes = ReadList(root, "frame", FrameGraphNode.FromYaml);

        TrackList(Samplers, nameof(Samplers));
        TrackList(Floats, nameof(Floats));
        TrackList(Vec4, nameof(Vec4));
        TrackList(RenderTargets, nameof(RenderTargets));
        TrackList(Nodes, nameof(Nodes));
    }

    void SaveRendererAsset()
    {
        var root = new YamlMappingNode
        {
            { "samplers", WriteList(Samplers, sampler => sampler.ToYaml()) },
            { "float", WriteNamedScalarList(Floats) },
            { "vec4", WriteNamedVec4List(Vec4) },
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

    static ObservableList<T> ReadList<T>(YamlMappingNode? root, string name, Func<YamlMappingNode, T> factory)
        where T : INotifyPropertyChanged
    {
        var result = new ObservableList<T>();
        if (root is null || !YamlHelper.TryGetSequence(root, name, out var sequence))
        {
            return result;
        }

        foreach (var item in sequence.Children.OfType<YamlMappingNode>())
        {
            result.Add(factory(item));
        }

        return result;
    }

    static ObservableList<FrameGraphScalar> ReadNamedScalarList(YamlMappingNode? root, string name)
    {
        var result = new ObservableList<FrameGraphScalar>();
        if (root is null || !YamlHelper.TryGetSequence(root, name, out var sequence))
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

    static ObservableList<FrameGraphVec4Value> ReadNamedVec4List(YamlMappingNode? root, string name)
    {
        var result = new ObservableList<FrameGraphVec4Value>();
        if (root is null || !YamlHelper.TryGetSequence(root, name, out var sequence))
        {
            return result;
        }

        foreach (var item in sequence.Children.OfType<YamlMappingNode>())
        {
            foreach (var entry in item.Children)
            {
                result.Add(new FrameGraphVec4Value
                {
                    Key = ScalarToString(entry.Key),
                    Value = FrameGraphVec4.FromYaml(entry.Value)
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

    static YamlSequenceNode WriteNamedVec4List(IEnumerable<FrameGraphVec4Value> values)
    {
        var sequence = new YamlSequenceNode();
        foreach (var value in values)
        {
            sequence.Add(new YamlMappingNode
            {
                { value.Key ?? string.Empty, value.Value?.ToYaml() ?? new FrameGraphVec4().ToYaml() }
            });
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
        Name = YamlHelper.ReadString(node, "name"),
        FileId = YamlHelper.ReadString(node, "fileId"),
        Path = YamlHelper.ReadString(node, "path")
    };

    public YamlMappingNode ToYaml() => new()
    {
        { "name", Name ?? string.Empty },
        { "fileId", FileId ?? string.Empty },
        { "path", Path ?? string.Empty }
    };

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
        Name = YamlHelper.ReadString(node, "name"),
        Format = YamlHelper.ReadString(node, "format", "R16G16B16A16_SFLOAT"),
        Width = YamlHelper.ReadString(node, "width", "ViewportWidth"),
        Height = YamlHelper.ReadString(node, "height", "ViewportHeight"),
        Clamping = YamlHelper.ReadString(node, "clamping"),
        Filtration = YamlHelper.ReadString(node, "filtration"),
        Reduction = YamlHelper.ReadString(node, "reduction"),
        IsSurface = YamlHelper.ReadBool(node, "bIsSurface"),
        IsCompatibleWithComputeShaders = YamlHelper.ReadBool(node, "bIsCompatibleWithComputeShaders"),
        GenerateMips = YamlHelper.ReadBool(node, "bGenerateMips"),
        MaxMipLevel = YamlHelper.ReadInt(node, "maxMipLevel")
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
    public FrameGraphVec4Value()
    {
        Value.PropertyChanged += OnComponentChanged;
    }

    [ObservableProperty]
    string key = string.Empty;

    [ObservableProperty]
    FrameGraphVec4 value = new();

    partial void OnValueChanging(FrameGraphVec4 value) =>
        Value.PropertyChanged -= OnComponentChanged;

    partial void OnValueChanged(FrameGraphVec4 value) =>
        value.PropertyChanged += OnComponentChanged;

    void OnComponentChanged(object? sender, PropertyChangedEventArgs args) =>
        OnPropertyChanged(nameof(Value));
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

    [ObservableProperty]
    string tag = string.Empty;

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
        var result = new FrameGraphNode
        {
            Name = YamlHelper.ReadString(node, "name"),
            Tag = YamlHelper.ReadString(node, "tag")
        };
        ReadNamedScalarList(node, "string", result.Strings);
        ReadNamedScalarList(node, "float", result.Floats);
        ReadVec4List(node, result.Vec4);
        ReadRenderTargets(node, result.RenderTargets);
        return result;
    }

    public YamlMappingNode ToYaml()
    {
        var node = new YamlMappingNode { { "name", Name ?? string.Empty } };
        if (!string.IsNullOrWhiteSpace(Tag))
        {
            node.Add("tag", Tag);
        }
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

    static void ReadNamedScalarList(YamlMappingNode node, string name, ObservableList<FrameGraphScalar> values)
    {
        if (!YamlHelper.TryGetSequence(node, name, out var sequence))
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
        if (!YamlHelper.TryGetSequence(node, "vec4", out var sequence))
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
        if (!YamlHelper.TryGetSequence(node, "renderTargets", out var sequence))
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
