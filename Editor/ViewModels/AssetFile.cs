using System.Runtime.CompilerServices;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using System.Diagnostics;
using CommunityToolkit.Mvvm.ComponentModel;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using SailorEngine;
using SailorEditor.Services;
using SailorEditor.Utility;
using YamlDotNet.RepresentationModel;
using System.Collections.Specialized;

namespace SailorEditor.ViewModels;

public partial class AssetFile : ObservableObject, ICloneable
{
    public AssetFile()
    {
        PropertyChanged += (s, args) =>
        {
            if (!suppressDirtyTracking &&
                args.PropertyName != nameof(IsDirty) &&
                args.PropertyName != nameof(CanOpenAssetFile) &&
                args.PropertyName != nameof(LoadError))
            {
                IsDirty = true;
            }
        };
    }

    [YamlIgnore]
    public FileInfo Asset { get; set; }

    [YamlIgnore]
    public FileInfo AssetInfo { get; set; }

    [YamlIgnore]
    public int Id { get; set; }

    [YamlIgnore]
    public int FolderId { get; set; }

    [YamlIgnore]
    public bool CanOpenAssetFile { get => !IsDirty; }

    [YamlIgnore]
    public ObservableDictionary<string, ObservableObject> AssetProperties { get; set; } = [];

    [YamlIgnore]
    public AssetType AssetType { get; set; }

    [YamlIgnore]
    public string AssetInfoTypeName { get; set; }

    [YamlIgnore]
    public bool HasRegisteredAssetType => AssetType != null;

    [YamlIgnore]
    public HashSet<string> ReadOnlyAssetProperties { get; } = [];

    [YamlIgnore]
    public HashSet<string> TransientAssetProperties { get; } = [];

    [YamlIgnore]
    readonly Dictionary<string, YamlNode> originalAssetInfoNodes = [];

    public virtual Task<bool> LoadDependentResources() => Task.FromResult(true);

    public virtual Task Save()
    {
        if (!AssetProperties.Any())
        {
            return Save(new AssetFileYamlConverter());
        }

        SyncAssetPropertiesBeforeSave();

        var yaml = new YamlStream(new YamlDocument(BuildAssetInfoYamlNode()));
        using var writer = new StreamWriter(new FileStream(AssetInfo.FullName, FileMode.Create));
        yaml.Save(writer, false);

        IsDirty = false;
        return Task.CompletedTask;
    }

    public virtual Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();
                DisplayName = Filename;
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

    protected void RunWithoutDirtyTracking(Action action)
    {
        suppressDirtyTracking = true;
        try
        {
            action();
        }
        finally
        {
            suppressDirtyTracking = false;
        }
    }

    protected void ResetDirtyState()
    {
        suppressDirtyTracking = true;
        try
        {
            IsDirty = false;
            OnPropertyChanged(nameof(AssetProperties));
        }
        finally
        {
            suppressDirtyTracking = false;
        }
    }

    protected virtual void SyncAssetPropertiesBeforeSave()
    {
        SetAssetPropertyValue("fileId", FileId?.Value);
        SetAssetPropertyValue("filename", Filename?.Value);
    }

    public void EnsureAssetPropertiesTyped()
    {
        if (AssetType != null || IsDirty)
        {
            return;
        }

        var resolvedAssetType = ResolveAssetType();
        if (resolvedAssetType == null)
        {
            return;
        }

        RunWithoutDirtyTracking(() =>
        {
            AssetType = resolvedAssetType;
            LoadAssetPropertiesFromAssetInfo();
        });
        ResetDirtyState();
    }

    protected void LoadAssetPropertiesFromAssetInfo()
    {
        var stream = new YamlStream();
        using (var reader = new StreamReader(AssetInfo.FullName))
        {
            stream.Load(reader);
        }

        var properties = new ObservableDictionary<string, ObservableObject>();
        originalAssetInfoNodes.Clear();
        ReadOnlyAssetProperties.Clear();
        TransientAssetProperties.Clear();
        if (stream.Documents.Count > 0 && stream.Documents[0].RootNode is YamlMappingNode root)
        {
            if (root.Children.TryGetValue(new YamlScalarNode("assetInfoType"), out var assetInfoType))
            {
                AssetInfoTypeName = FormatYamlFieldValue(assetInfoType);
            }

            AssetType = ResolveAssetType();
            foreach (var entry in root.Children)
            {
                var name = (entry.Key as YamlScalarNode)?.Value ?? entry.Key.ToString();
                PropertyBase property = null;
                AssetType?.Properties.TryGetValue(name, out property);
                originalAssetInfoNodes[name] = entry.Value;
                if (property == null)
                {
                    properties[name] = new Observable<string>(FormatYamlFieldValue(entry.Value));
                    ReadOnlyAssetProperties.Add(name);
                }
                else
                {
                    properties[name] = CreateObservableProperty(property, entry.Value);
                }
            }
        }
        else
        {
            AssetType = ResolveAssetType();
        }

        if (AssetType != null)
        {
            foreach (var property in AssetType.Properties)
            {
                if (!properties.ContainsKey(property.Key))
                {
                    properties[property.Key] = CreateDefaultObservableProperty(property.Value);
                }
            }
        }

        AddTransientAssetProperties(properties);

        AssetProperties = properties;
        AssetProperties.CollectionChanged += (a, e) => MarkDirty(nameof(AssetProperties));
        AssetProperties.PropertyChanged += (a, e) => MarkDirty(nameof(AssetProperties));
        AssetProperties.ValueChanged += (a, e) => MarkDirty(nameof(AssetProperties));
        OnPropertyChanged(nameof(AssetProperties));

        FileId = new FileId(GetAssetPropertyValue("fileId") ?? string.Empty);
        Filename = new FileId(GetAssetPropertyValue("filename") ?? string.Empty);
    }

    protected virtual void AddTransientAssetProperties(ObservableDictionary<string, ObservableObject> properties)
    {
    }

    protected AssetType ResolveAssetType()
    {
        var extension = Asset?.Extension?.TrimStart('.').ToLowerInvariant();
        var engineTypes = MauiProgram.GetService<EngineService>()?.EngineTypes;
        if (!string.IsNullOrEmpty(AssetInfoTypeName) && engineTypes?.AssetTypes?.TryGetValue(AssetInfoTypeName, out var typedAssetType) == true)
        {
            return typedAssetType;
        }

        if (!string.IsNullOrEmpty(extension) && engineTypes?.AssetTypesByExtension?.TryGetValue(extension, out var assetType) == true)
        {
            return assetType;
        }

        AssetType defaultType = null;
        engineTypes?.AssetTypes?.TryGetValue("Sailor::AssetInfo", out defaultType);
        return defaultType;
    }

    protected string GetAssetPropertyValue(string fieldName) =>
        AssetProperties.TryGetValue(fieldName, out var value) ? ObservableToString(value) : null;

    protected void SetAssetPropertyValue(string fieldName, string value)
    {
        if (AssetProperties.TryGetValue(fieldName, out var property))
        {
            switch (property)
            {
                case Observable<string> observableString:
                    observableString.Value = value ?? string.Empty;
                    break;
                case Observable<FileId> observableFileId:
                    observableFileId.Value = new FileId(value ?? string.Empty);
                    break;
                case Observable<int> observableInt:
                    observableInt.Value = int.TryParse(value, out var parsedInt) ? parsedInt : 0;
                    break;
                case Observable<float> observableFloat:
                    observableFloat.Value = float.TryParse(value, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsedFloat) ? parsedFloat : 0.0f;
                    break;
                case Observable<bool> observableBool:
                    observableBool.Value = bool.TryParse(value, out var parsedBool) && parsedBool;
                    break;
            }
        }
    }

    protected static int GetIntAssetProperty(ObservableDictionary<string, ObservableObject> fields, string fieldName, int fallback = 0)
    {
        return fields.TryGetValue(fieldName, out var value) && value is Observable<int> observableInt ? observableInt.Value : fallback;
    }

    protected static bool GetBoolAssetProperty(ObservableDictionary<string, ObservableObject> fields, string fieldName, bool fallback = false)
    {
        return fields.TryGetValue(fieldName, out var value) && value is Observable<bool> observableBool ? observableBool.Value : fallback;
    }

    protected static float GetFloatAssetProperty(ObservableDictionary<string, ObservableObject> fields, string fieldName, float fallback = 0.0f)
    {
        return fields.TryGetValue(fieldName, out var value) && value is Observable<float> observableFloat ? observableFloat.Value : fallback;
    }

    YamlMappingNode BuildAssetInfoYamlNode()
    {
        var root = new YamlMappingNode();
        foreach (var field in AssetProperties)
        {
            if (TransientAssetProperties.Contains(field.Key))
            {
                continue;
            }

            var key = new YamlScalarNode(field.Key);
            if (ReadOnlyAssetProperties.Contains(field.Key))
            {
                root.Add(key, originalAssetInfoNodes.TryGetValue(field.Key, out var original) ? original : new YamlScalarNode(ObservableToString(field.Value)));
            }
            else
            {
                root.Add(key, CreateYamlNode(field.Value));
            }
        }
        return root;
    }

    static YamlNode CreateYamlNode(ObservableObject property)
    {
        switch (property)
        {
            case Observable<string> observableString:
                return observableString.Value == "~" ? new YamlScalarNode(null) : new YamlScalarNode(observableString.Value ?? string.Empty);
            case Observable<FileId> observableFileId:
                return new YamlScalarNode(observableFileId.Value?.Value ?? FileId.NullFileId);
            case Observable<bool> observableBool:
                return new YamlScalarNode(observableBool.Value.ToString().ToLowerInvariant());
            case Observable<int> observableInt:
                return new YamlScalarNode(observableInt.Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
            case Observable<float> observableFloat:
                return new YamlScalarNode(observableFloat.Value.ToString(System.Globalization.CultureInfo.InvariantCulture));
            case ObservableFileIdList fileIds:
            {
                var sequence = new YamlSequenceNode();
                foreach (var fileId in fileIds.Values)
                {
                    sequence.Add(new YamlScalarNode(fileId.Value?.Value ?? FileId.NullFileId));
                }
                return sequence;
            }
            default:
                return new YamlScalarNode(ObservableToString(property));
        }
    }

    static string FormatYamlFieldValue(YamlNode node)
    {
        if (node is YamlScalarNode scalar)
        {
            return scalar.Value ?? "~";
        }

        using var writer = new StringWriter();
        new YamlStream(new YamlDocument(node)).Save(writer, false);
        return writer.ToString().TrimEnd();
    }

    static ObservableObject CreateObservableProperty(PropertyBase property, YamlNode node)
    {
        return property switch
        {
            FileIdProperty => new Observable<FileId>((node as YamlScalarNode)?.Value ?? FileId.NullFileId),
            Property<bool> => new Observable<bool>(bool.TryParse((node as YamlScalarNode)?.Value, out var parsed) && parsed),
            Property<int> => new Observable<int>(int.TryParse((node as YamlScalarNode)?.Value, out var parsed) ? parsed : 0),
            FloatProperty => new Observable<float>(float.TryParse((node as YamlScalarNode)?.Value, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsed) ? parsed : 0.0f),
            EnumProperty => new Observable<string>((node as YamlScalarNode)?.Value ?? string.Empty),
            Property<string> => new Observable<string>((node as YamlScalarNode)?.Value ?? string.Empty),
            Property<List<FileId>> => CreateFileIdList(node),
            _ => new Observable<string>(FormatYamlFieldValue(node))
        };
    }

    static ObservableObject CreateDefaultObservableProperty(PropertyBase property)
    {
        return property switch
        {
            FileIdProperty => new Observable<FileId>(FileId.NullFileId),
            Property<bool> => new Observable<bool>(false),
            Property<int> => new Observable<int>(0),
            FloatProperty => new Observable<float>(0.0f),
            EnumProperty => new Observable<string>(string.Empty),
            Property<string> => new Observable<string>(string.Empty),
            Property<List<FileId>> => new ObservableFileIdList(),
            _ => new Observable<string>(string.Empty)
        };
    }

    static ObservableFileIdList CreateFileIdList(YamlNode node)
    {
        var res = new ObservableFileIdList();
        if (node is YamlSequenceNode sequence)
        {
            foreach (var child in sequence.Children.OfType<YamlScalarNode>())
            {
                res.Values.Add(new Observable<FileId>(child.Value ?? FileId.NullFileId));
            }
        }
        return res;
    }

    static string ObservableToString(ObservableObject property) => property switch
    {
        Observable<string> observableString => observableString.Value ?? string.Empty,
        Observable<FileId> observableFileId => observableFileId.Value?.Value ?? FileId.NullFileId,
        Observable<bool> observableBool => observableBool.Value.ToString().ToLowerInvariant(),
        Observable<int> observableInt => observableInt.Value.ToString(System.Globalization.CultureInfo.InvariantCulture),
        Observable<float> observableFloat => observableFloat.Value.ToString(System.Globalization.CultureInfo.InvariantCulture),
        ObservableFileIdList fileIds => string.Join(", ", fileIds.Values.Select(fileId => fileId.Value?.Value ?? FileId.NullFileId)),
        _ => property?.ToString() ?? string.Empty
    };

    protected void SetLoadError(Exception ex)
    {
        LoadError = ex.Message;

        if (string.IsNullOrWhiteSpace(DisplayName) || DisplayName == LoadError)
        {
            DisplayName = Asset?.Name ?? AssetInfo?.Name ?? Filename?.Value ?? FileId?.Value ?? GetType().Name;
        }

        var message = $"[{GetType().Name}] {DisplayName}: {ex.Message}";
        Console.WriteLine(message);
        MauiProgram.GetService<EngineService>()?.PushConsoleMessage(message);
    }

    public void Open()
    {
        var path = Asset?.Exists == true ? Asset.FullName : AssetInfo.FullName;
        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
    }

    public void MarkDirty([CallerMemberName] string propertyName = null) { IsDirty = true; OnPropertyChanged(propertyName); }

    public object Clone() => new AssetFile();

    public bool IsLoaded { get; set; }

    [YamlIgnore]
    public bool IsDirty
    {
        get => isDirty;
        set
        {
            if (SetProperty(ref isDirty, value))
            {
                OnPropertyChanged(nameof(CanOpenAssetFile));
            }
        }
    }

    [YamlIgnore]
    protected bool isDirty = false;

    [YamlIgnore]
    bool suppressDirtyTracking = false;

    [ObservableProperty]
    protected string displayName;

    [ObservableProperty]
    string loadError = string.Empty;

    [ObservableProperty]
    FileId fileId;

    [ObservableProperty]
    FileId filename;

    protected Task Save(IYamlTypeConverter converter)
    {
        using (var yamlAssetInfo = new FileStream(AssetInfo.FullName, FileMode.Create))
        using (var writer = new StreamWriter(yamlAssetInfo))
        {
            var serializer = SerializationUtils.CreateSerializerBuilder()
                .WithTypeConverter(converter)
                .Build();

            var yaml = serializer.Serialize(this);
            writer.Write(yaml);
        }

        IsDirty = false;
        return Task.CompletedTask;
    }
}

public partial class ObservableFileIdList : ObservableObject
{
    public ObservableFileIdList()
    {
        Values.CollectionChanged += ValuesCollectionChanged;
        Values.ItemChanged += ValuesItemChanged;
    }

    public ObservableList<Observable<FileId>> Values { get; } = [];

    void ValuesCollectionChanged(object sender, NotifyCollectionChangedEventArgs e)
    {
        OnPropertyChanged(nameof(Values));
    }

    void ValuesItemChanged(object sender, ItemChangedEventArgs<Observable<FileId>> e)
    {
        OnPropertyChanged(nameof(Values));
    }
}

public class AssetFileYamlConverter : IYamlTypeConverter
{
    public bool Accepts(Type type) => type == typeof(AssetFile);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .Build();

        var assetFile = new AssetFile();

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
                        assetFile.FileId = deserializer.Deserialize<FileId>(parser);
                        break;
                    case "filename":
                        assetFile.Filename = deserializer.Deserialize<string>(parser);
                        break;
                    default:
                        // Skip other properties
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

        return assetFile;
    }

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
        var assetFile = (AssetFile)value;

        emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

        emitter.Emit(new Scalar(null, "fileId"));
        emitter.Emit(new Scalar(null, assetFile.FileId));

        emitter.Emit(new Scalar(null, "filename"));
        emitter.Emit(new Scalar(null, assetFile.Filename));

        emitter.Emit(new MappingEnd());
    }
}
