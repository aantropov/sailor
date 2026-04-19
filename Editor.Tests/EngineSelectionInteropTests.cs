using SailorEditor.Services;
using SailorEngine;

namespace Editor.Tests;

public class EngineSelectionInteropTests
{
    [Fact]
    public void BuildEditorSelectionYamlFiltersEmptyIds()
    {
        var yaml = EngineService.BuildEditorSelectionYaml([
            new InstanceId("go-1"),
            InstanceId.NullInstanceId,
            new InstanceId(""),
            new InstanceId("cmp-2")
        ]);

        Assert.Contains("- go-1", yaml);
        Assert.Contains("- cmp-2", yaml);
        Assert.DoesNotContain("NullInstanceId", yaml);
    }
}
