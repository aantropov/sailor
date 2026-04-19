using SailorEditor.Settings;

namespace SailorEditor.Editor.Tests;

public class UnifiedSettingsStoreTests
{
    [Fact]
    public async Task Resolve_PrefersProjectThenEditorThenEngine()
    {
        var store = new UnifiedSettingsStore(EditorSettingsCatalog.Definitions);
        var entry = EditorSettingsCatalog.Definitions.Single(x => x.Entry.Key == "engine.vsync").Entry;

        await store.SetValueAsync(entry, SettingsScope.Engine, false);
        Assert.False((bool)store.Resolve(entry).Value!);

        await store.SetValueAsync(entry, SettingsScope.Editor, true);
        var editorResolved = store.Resolve(entry);
        Assert.True((bool)editorResolved.Value!);
        Assert.Equal(SettingsScope.Editor, editorResolved.SourceScope);

        await store.SetValueAsync(entry, SettingsScope.Project, false);
        var projectResolved = store.Resolve(entry);
        Assert.False((bool)projectResolved.Value!);
        Assert.Equal(SettingsScope.Project, projectResolved.SourceScope);
    }

    [Fact]
    public void Validate_RejectsInvalidTypesAndOutOfRangeValues()
    {
        var store = new UnifiedSettingsStore(EditorSettingsCatalog.Definitions);
        var renderScale = EditorSettingsCatalog.Definitions.Single(x => x.Entry.Key == "engine.renderScale").Entry;

        Assert.False(store.Validate(renderScale, "fast").IsValid);
        Assert.False(store.Validate(renderScale, 5.0).IsValid);
        Assert.True(store.Validate(renderScale, 1.25).IsValid);
    }

    [Fact]
    public async Task SaveAndLoad_RoundTripsYamlValues()
    {
        var path = Path.Combine(Path.GetTempPath(), $"sailor-editor-settings-{Guid.NewGuid():N}.yaml");
        try
        {
            var store = new UnifiedSettingsStore(EditorSettingsCatalog.Definitions);
            var theme = EditorSettingsCatalog.Definitions.Single(x => x.Entry.Key == "editor.theme").Entry;
            await store.SetValueAsync(theme, SettingsScope.Editor, "Light");
            await store.SaveAsync(path);

            var reloaded = new UnifiedSettingsStore(EditorSettingsCatalog.Definitions);
            await reloaded.LoadAsync(path);

            var resolved = reloaded.Resolve(theme);
            Assert.Equal("Light", resolved.Value);
            Assert.Equal(SettingsScope.Editor, resolved.SourceScope);
        }
        finally
        {
            if (File.Exists(path))
                File.Delete(path);
        }
    }
}
