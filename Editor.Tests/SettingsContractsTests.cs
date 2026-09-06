using SailorEditor.Settings;

namespace SailorEditor.Editor.Tests;

public class SettingsContractsTests
{
    [Fact]
    public void ShadowDistance_RoundTripsThroughYamlAndEditorDraft()
    {
        var preset = GraphicsSettingsDefaults.Project.Graphics.Presets.Ultra with
        {
            ShadowDistance = 600
        };
        var source = GraphicsSettingsDefaults.Project with
        {
            Graphics = GraphicsSettingsDefaults.Project.Graphics with
            {
                Presets = GraphicsSettingsDefaults.Project.Graphics.Presets.With(
                    GraphicsQualityLevel.Ultra, preset)
            }
        };
        var diagnostics = new List<string>();
        var parsed = GraphicsSettingsYamlCodec.ParseProject(
            GraphicsSettingsYamlCodec.SerializeProject(source), diagnostics, "shadow-distance");
        Assert.Empty(diagnostics);
        Assert.Equal(1, parsed.SettingsVersion);
        Assert.Equal(600, parsed.Graphics.Presets.Ultra.ShadowDistance);
        Assert.Equal(200, parsed.Graphics.Presets.High.ShadowDistance);

        var draft = GraphicsQualityPresetDraft.FromSettings(parsed.Graphics.Presets.Ultra);
        var issues = new List<GraphicsSettingsValidationIssue>();
        Assert.True(draft.TryBuild(GraphicsQualityLevel.Ultra, out var built, issues));
        Assert.Empty(issues);
        Assert.Equal(preset.ShadowDistance, built.ShadowDistance);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(10001)]
    [InlineData(double.NaN)]
    [InlineData(double.PositiveInfinity)]
    public void ShadowDistance_RejectsNonFiniteOrOutOfRangeValues(double distance)
    {
        var source = GraphicsSettingsDefaults.Project with
        {
            Graphics = GraphicsSettingsDefaults.Project.Graphics with
            {
                Presets = GraphicsSettingsDefaults.Project.Graphics.Presets.With(
                    GraphicsQualityLevel.Ultra,
                    GraphicsSettingsDefaults.Project.Graphics.Presets.Ultra with { ShadowDistance = distance })
            }
        };
        var result = GraphicsSettingsValidator.Validate(source);
        Assert.False(result.IsValid);
        Assert.Contains(result.Issues, issue => issue.Path == "graphics.presets.Ultra.shadowDistance");
    }

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

    [Fact]
    public void RuntimeGISettings_RoundTripThroughStructuredYaml()
    {
        var runtime = new RuntimeGIProbesQualitySettings
        {
            Enabled = true,
            MaxActiveProbes = 4096,
            SpacingMultiplier = 1.5,
            InitialSamplesPerProbe = 16,
            TargetSamplesPerProbe = 32,
            WorkerCount = 16,
            CpuDutyFraction = 0.2,
            CpuBudgetMilliseconds = 2.0,
            MaxPublicationsPerSecond = 1.0,
            InitialPublicationCoverage = 0.25,
            MaxDirtyUploadBytesPerFrame = 1024 * 1024
        };
        var source = GraphicsSettingsDefaults.Project with
        {
            Graphics = GraphicsSettingsDefaults.Project.Graphics with
            {
                Presets = GraphicsSettingsDefaults.Project.Graphics.Presets.With(
                    GraphicsQualityLevel.High,
                    GraphicsSettingsDefaults.Project.Graphics.Presets.High with
                    {
                        RuntimeGIProbes = runtime
                    })
            }
        };

        var yaml = GraphicsSettingsYamlCodec.SerializeProject(source);
        var diagnostics = new List<string>();
        var parsed = GraphicsSettingsYamlCodec.ParseProject(
            yaml,
            diagnostics,
            "runtime-gi-project-settings");

        Assert.Empty(diagnostics);
        Assert.Equal(1, parsed.SettingsVersion);
        Assert.Equal(runtime, parsed.Graphics.Presets.High.RuntimeGIProbes);
    }

    [Fact]
    public void RuntimeGIPreview_RoundTripsAsWorkspaceLocalState()
    {
        var source = GraphicsSettingsDefaults.Editor with
        {
            Graphics = GraphicsSettingsDefaults.Editor.Graphics with
            {
                RuntimeGIProbesPreviewEnabled = true,
                RuntimeGIProbesBudget = RuntimeGIProbesEditorBudget.Balanced,
                RuntimeGIProbesDebugView =
                    RuntimeGIProbesEditorDebugView.Subdivisions
            }
        };

        var yaml = GraphicsSettingsYamlCodec.SerializeEditor(source);
        var diagnostics = new List<string>();
        var parsed = GraphicsSettingsYamlCodec.ParseEditor(
            yaml,
            diagnostics,
            "runtime-gi-editor-settings");

        Assert.Empty(diagnostics);
        Assert.Equal(1, parsed.SettingsVersion);
        Assert.True(parsed.Graphics.RuntimeGIProbesPreviewEnabled);
        Assert.Equal(
            RuntimeGIProbesEditorBudget.Balanced,
            parsed.Graphics.RuntimeGIProbesBudget);
        Assert.Equal(
            RuntimeGIProbesEditorDebugView.Subdivisions,
            parsed.Graphics.RuntimeGIProbesDebugView);
    }
}
