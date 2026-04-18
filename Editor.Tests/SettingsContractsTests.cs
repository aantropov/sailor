using SailorEditor.Settings;

namespace SailorEditor.Editor.Tests;

public class SettingsContractsTests
{
    [Fact]
    public void SettingsCategory_ExposesHierarchyAndEntries()
    {
        var entry = new SettingsEntry("engine.vsync", "VSync", SettingsValueKind.Boolean, SettingsScope.Engine);
        var child = new SettingsCategory("rendering", "Rendering", Entries: [entry]);
        var root = new SettingsCategory("root", "Root", Children: [child]);

        var childCategory = Assert.Single(root.Children!);
        var childEntry = Assert.Single(childCategory.Entries!);

        Assert.Equal(SettingsScope.Engine, childEntry.Scope);
    }

    [Fact]
    public void SettingsValidationResult_IsInvalidWhenAnyErrorExists()
    {
        var result = new SettingsValidationResult(
            [
                new SettingsValidationMessage("engine.vsync", "Need restart", SettingsValidationSeverity.Warning),
                new SettingsValidationMessage("engine.backend", "Unsupported backend", SettingsValidationSeverity.Error),
            ]);

        Assert.False(result.IsValid);
        Assert.False(SettingsValidationResult.Success.Messages.Any());
        Assert.True(SettingsValidationResult.Success.IsValid);
    }
}
