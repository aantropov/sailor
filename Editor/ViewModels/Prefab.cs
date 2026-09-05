#nullable enable
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using System.ComponentModel;
using SailorEngine;

namespace SailorEditor.ViewModels;

public sealed class PrefabGameObjectOverride : ICloneable
{
    [YamlMember(DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public string? Name { get; set; }

    [YamlMember(DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public string? MobilityType { get; set; }

    [YamlMember(DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public Vec4? Position { get; set; }

    [YamlMember(DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public Rotation? Rotation { get; set; }

    [YamlMember(DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public Vec4? Scale { get; set; }

    public object Clone() => new PrefabGameObjectOverride
    {
        Name = Name,
        MobilityType = MobilityType,
        Position = Position is null
            ? null
            : (Vec4)Position.Clone(),
        Rotation = Rotation is null
            ? null
            : (Rotation)Rotation.Clone(),
        Scale = Scale is null
            ? null
            : (Vec4)Scale.Clone()
    };
}

public sealed class PrefabComponentOverride : ICloneable
{
    [YamlMember(DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public string? Typename { get; set; }

    [YamlMember(DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public Dictionary<string, object?>? OverrideProperties { get; set; }

    public object Clone() => new PrefabComponentOverride
    {
        Typename = Typename,
        OverrideProperties = OverrideProperties is null
            ? null
            : new Dictionary<string, object?>(
                OverrideProperties,
                StringComparer.Ordinal)
    };
}

public partial class Prefab : ObservableObject, ICloneable
{
    public object Clone() => new Prefab()
    {
        FileId = FileId is null
            ? null
            : (FileId)FileId.Clone(),
        DetachedFromPrefab = DetachedFromPrefab,
        LinkedPrefabSnapshot = LinkedPrefabSnapshot,
        ParentInstanceId = ParentInstanceId,
        InstanceIds = InstanceIds is null
            ? null
            : new Dictionary<string, string>(
                InstanceIds,
                StringComparer.Ordinal),
        GameObjectOverrides = GameObjectOverrides?
            .ToDictionary(
                entry => entry.Key,
                entry => (PrefabGameObjectOverride)entry.Value.Clone(),
                StringComparer.Ordinal),
        ComponentOverrides = ComponentOverrides?
            .ToDictionary(
                entry => entry.Key,
                entry => (PrefabComponentOverride)entry.Value.Clone(),
                StringComparer.Ordinal),
        GameObjects = new ObservableList<GameObject>(GameObjects),
        Components = new ObservableList<Component>(Components)
    };

    [YamlMember(
        Alias = "fileId",
        DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public FileId? FileId { get; set; }

    [YamlIgnore]
    public bool IsLinked => FileId is not null && !FileId.IsEmpty();

    [YamlMember(
        Alias = "detachedFromPrefab",
        DefaultValuesHandling = DefaultValuesHandling.OmitDefaults)]
    public bool DetachedFromPrefab { get; set; }

    [YamlMember(
        Alias = "linkedPrefabSnapshot",
        DefaultValuesHandling = DefaultValuesHandling.OmitDefaults)]
    public bool LinkedPrefabSnapshot { get; set; }

    [YamlMember(
        Alias = "parentInstanceId",
        DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public string? ParentInstanceId { get; set; }

    [YamlMember(
        Alias = "instanceIds",
        DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public Dictionary<string, string>? InstanceIds { get; set; }

    [YamlMember(
        Alias = "gameObjectOverrides",
        DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public Dictionary<string, PrefabGameObjectOverride>? GameObjectOverrides
    {
        get;
        set;
    }

    [YamlMember(
        Alias = "componentOverrides",
        DefaultValuesHandling = DefaultValuesHandling.OmitNull)]
    public Dictionary<string, PrefabComponentOverride>? ComponentOverrides
    {
        get;
        set;
    }

    [ObservableProperty]
    ObservableList<GameObject> gameObjects = [];

    [ObservableProperty]
    ObservableList<Component> components = [];
}
