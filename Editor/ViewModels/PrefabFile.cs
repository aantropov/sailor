using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.ViewModels;

public partial class PrefabFile : AssetFile
{
    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();
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

    protected override void AddTransientAssetProperties(ObservableDictionary<string, ObservableObject> properties)
    {
        var (gameObjects, components) = PrefabWorldStats.ReadDirectPrefabStats(Asset);
        PrefabWorldStats.AddReadOnlyStat(properties, ReadOnlyAssetProperties, TransientAssetProperties, "gameObjectsCount", gameObjects.ToString());
        PrefabWorldStats.AddReadOnlyStat(properties, ReadOnlyAssetProperties, TransientAssetProperties, "componentsCount", components.ToString());
    }
}

static class PrefabWorldStats
{
    public static void AddReadOnlyStat(ObservableDictionary<string, ObservableObject> properties,
        HashSet<string> readOnlyProperties,
        HashSet<string> transientProperties,
        string name,
        string value)
    {
        properties[name] = new Observable<string>(value);
        readOnlyProperties.Add(name);
        transientProperties.Add(name);
    }

    public static (int gameObjects, int components) ReadDirectPrefabStats(FileInfo asset)
    {
        return CountPrefabNode(ReadRoot(asset));
    }

    public static (int gameObjects, int components) ReadWorldStats(FileInfo asset)
    {
        var root = ReadRoot(asset);
        var prefabs = (YamlSequenceNode)root.Children[new YamlScalarNode("prefabs")];

        int gameObjects = 0;
        int components = 0;
        foreach (var prefabNode in prefabs.Children.OfType<YamlMappingNode>())
        {
            var (prefabGameObjects, prefabComponents) = CountPrefabNode(prefabNode);
            gameObjects += prefabGameObjects;
            components += prefabComponents;
        }

        return (gameObjects, components);
    }

    static (int gameObjects, int components) CountPrefabNode(YamlMappingNode prefabNode)
    {
        var gameObjects = (YamlSequenceNode)prefabNode.Children[new YamlScalarNode("gameObjects")];
        var components = (YamlSequenceNode)prefabNode.Children[new YamlScalarNode("components")];
        return (gameObjects.Children.Count, components.Children.Count);
    }

    static YamlMappingNode ReadRoot(FileInfo asset)
    {
        var stream = new YamlStream();
        using (var reader = new StreamReader(asset.FullName))
        {
            stream.Load(reader);
        }

        return (YamlMappingNode)stream.Documents[0].RootNode;
    }
}
