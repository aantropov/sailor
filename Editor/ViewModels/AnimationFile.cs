using CommunityToolkit.Mvvm.ComponentModel;

namespace SailorEditor.ViewModels;

public partial class AnimationFile : AssetFile
{
    [ObservableProperty]
    int animationIndex;

    [ObservableProperty]
    int skinIndex;

    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();
                AnimationIndex = GetIntAssetProperty(AssetProperties, "animationIndex");
                SkinIndex = GetIntAssetProperty(AssetProperties, "skinIndex");
                DisplayName = AssetInfo.Name;
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

    protected override void SyncAssetPropertiesBeforeSave()
    {
        base.SyncAssetPropertiesBeforeSave();
        SetAssetPropertyValue("animationIndex", AnimationIndex.ToString());
        SetAssetPropertyValue("skinIndex", SkinIndex.ToString());
    }
}
