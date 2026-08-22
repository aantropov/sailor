namespace SailorEditor.Editor.Tests;

public sealed class GraphicsSettingsUiContractTests
{
    [Fact]
    public void SceneView_ExposesSynchronizedQualityAndStatsSelectors()
    {
        var xaml = ReadRepositoryFile("Editor", "Views", "SceneView.xaml");
        var source = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");

        Assert.Contains("x:Name=\"QualityPicker\"", xaml);
        Assert.Contains("x:Name=\"StatsModePicker\"", xaml);
        Assert.Contains("OnQualitySelectedIndexChanged", xaml);
        Assert.Contains("OnStatsModeSelectedIndexChanged", xaml);
        Assert.Contains("SetSelectedQualityAsync", source);
        Assert.Contains("SetStatsModeAsync", source);
        Assert.Contains("graphicsSettingsService.SettingsChanged +=", source);
        Assert.Contains("workspaceUiService.ProjectionChanged +=", source);
    }

    [Fact]
    public void SettingsPanel_EditsEveryFixedPresetFieldThroughSharedService()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Views",
            "SettingsPanelView.cs");

        Assert.Contains("Project Default Quality", source);
        Assert.Contains("Scene View Quality", source);
        Assert.Contains("Scene View Stats", source);
        Assert.Contains("Resolution Factor", source);
        Assert.Contains("FPS Cap", source);
        Assert.Contains("MSAA", source);
        Assert.Contains("Shadow Quality Cap", source);
        Assert.Contains("Shadow Bias", source);
        Assert.Contains("Shadow Cascade Count", source);
        Assert.Contains("Cascade Resolutions", source);
        Assert.Contains("Soft Shadows", source);
        Assert.Contains("Clouds Resolution Multiplier", source);
        Assert.Contains("Sky Resolution", source);
        Assert.Contains("LOD Bias", source);
        Assert.Contains("Enum.GetValues<GraphicsQualityLevel>()", source);
        Assert.Contains("_graphicsSettings.ApplyAsync", source);
        Assert.Contains("draft.TryBuild(", source);
    }

    [Fact]
    public void SettingsPanel_UsesBoundedScrollingAndCollapsibleCompactPresets()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Views",
            "SettingsPanelView.cs");

        Assert.Contains("VerticalScrollBarVisibility = ScrollBarVisibility.Always", source);
        Assert.Contains(
            "new RowDefinition(GridLength.Auto),\n                " +
            "new RowDefinition(GridLength.Auto),\n                " +
            "new RowDefinition(GridLength.Star)\n            ]",
            source);
        Assert.DoesNotContain(
            "new RowDefinition(GridLength.Auto),\n                " +
            "new RowDefinition(GridLength.Auto),\n                " +
            "new RowDefinition(GridLength.Auto),\n                " +
            "new RowDefinition(GridLength.Star)",
            source);
        Assert.Contains("fields.IsVisible = initiallyExpanded", source);
        Assert.Contains("quality == snapshot.EffectiveQuality", source);
    }

    [Fact]
    public void DependencyInjection_WiresRestartAndLiveStatsEffects()
    {
        var source = ReadRepositoryFile("Editor", "MauiProgram.cs");

        Assert.Contains("new GraphicsSettingsService", source);
        Assert.Contains("RestartEngineForGenerationAsync", source);
        Assert.Contains("SetEditorStatsModeAsync(mode, token)", source);
        Assert.Contains("serializeWorkspaceMutationAsync", source);
        Assert.Contains("activationCoordinator.State.Generation", source);
    }

    static string ReadRepositoryFile(params string[] pathParts)
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var path = Path.Combine(
                new[] { current.FullName }.Concat(pathParts).ToArray());
            if (File.Exists(path))
                return File.ReadAllText(path).ReplaceLineEndings("\n");
            current = current.Parent;
        }

        throw new FileNotFoundException(
            $"Could not locate repository file '{Path.Combine(pathParts)}'.");
    }
}
