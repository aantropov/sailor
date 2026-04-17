using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;

namespace SailorEditor.ViewModels;

public partial class WorldFile : AssetFile
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
        var (gameObjects, components) = PrefabWorldStats.ReadWorldStats(Asset);
        PrefabWorldStats.AddReadOnlyStat(properties, ReadOnlyAssetProperties, TransientAssetProperties, "gameObjectsCount", gameObjects.ToString());
        PrefabWorldStats.AddReadOnlyStat(properties, ReadOnlyAssetProperties, TransientAssetProperties, "componentsCount", components.ToString());
    }
}
