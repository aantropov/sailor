using System.Globalization;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Settings;

public enum GraphicsQualityLevel
{
    Ultra,
    High,
    Medium,
    Low,
    VeryLow,
}

public enum EditorQualitySelection
{
    ProjectDefault,
    Ultra,
    High,
    Medium,
    Low,
    VeryLow,
}

public enum GraphicsShadowQuality
{
    High,
    Medium,
    Low,
    VeryLow,
}

public enum GraphicsStatsMode
{
    None,
    RenderStats,
    RenderStatsAndQueries,
}

public sealed record GraphicsQualityPresetSettings
{
    public double ResolutionFactor { get; init; }
    public int FpsCap { get; init; }
    public int MsaaSamples { get; init; }
    public GraphicsShadowQuality ShadowQuality { get; init; }
    public double ShadowBias { get; init; }
    public int ShadowCascadeCount { get; init; }
    public IReadOnlyList<int> ShadowCascadeResolutions { get; init; } = [];
    public bool SupportSoftShadows { get; init; }
    public double CloudsResolutionMultiplier { get; init; }
    public int SkyResolution { get; init; }
    public int VegetationInstanceBudget { get; init; }
    public int LodBias { get; init; }
    public int MaxGiProbeStatesPerSnapshot { get; init; }
}

public sealed record GraphicsQualityPresets
{
    public GraphicsQualityPresetSettings Ultra { get; init; } = new();
    public GraphicsQualityPresetSettings High { get; init; } = new();
    public GraphicsQualityPresetSettings Medium { get; init; } = new();
    public GraphicsQualityPresetSettings Low { get; init; } = new();
    public GraphicsQualityPresetSettings VeryLow { get; init; } = new();

    public GraphicsQualityPresetSettings Get(GraphicsQualityLevel quality)
        => quality switch
        {
            GraphicsQualityLevel.Ultra => Ultra,
            GraphicsQualityLevel.High => High,
            GraphicsQualityLevel.Medium => Medium,
            GraphicsQualityLevel.Low => Low,
            GraphicsQualityLevel.VeryLow => VeryLow,
            _ => throw new ArgumentOutOfRangeException(nameof(quality))
        };

    public GraphicsQualityPresets With(
        GraphicsQualityLevel quality,
        GraphicsQualityPresetSettings settings)
        => quality switch
        {
            GraphicsQualityLevel.Ultra => this with { Ultra = settings },
            GraphicsQualityLevel.High => this with { High = settings },
            GraphicsQualityLevel.Medium => this with { Medium = settings },
            GraphicsQualityLevel.Low => this with { Low = settings },
            GraphicsQualityLevel.VeryLow => this with { VeryLow = settings },
            _ => throw new ArgumentOutOfRangeException(nameof(quality))
        };
}

public sealed record ProjectGraphicsSettings
{
    public GraphicsQualityLevel DefaultQuality { get; init; } = GraphicsQualityLevel.High;
    public GraphicsQualityPresets Presets { get; init; } = GraphicsSettingsDefaults.Presets;
}

public sealed record ProjectSettingsDocument
{
    public const int CurrentVersion = 2;
    public const int LegacyVersion = 1;

    public int SettingsVersion { get; init; } = CurrentVersion;
    public ProjectGraphicsSettings Graphics { get; init; } = new();
}

public sealed record EditorGraphicsSettings
{
    public EditorQualitySelection SelectedQuality { get; init; } = EditorQualitySelection.ProjectDefault;
    public GraphicsStatsMode StatsMode { get; init; } = GraphicsStatsMode.None;
}

public sealed record WorkspaceEditorSettingsDocument
{
    public const int CurrentVersion = 1;

    public int SettingsVersion { get; init; } = CurrentVersion;
    public EditorGraphicsSettings Graphics { get; init; } = new();
}

public static class GraphicsSettingsDefaults
{
    public static GraphicsQualityPresets Presets { get; } = new()
    {
        Ultra = new GraphicsQualityPresetSettings
        {
            ResolutionFactor = 1.0,
            FpsCap = 120,
            MsaaSamples = 8,
            ShadowQuality = GraphicsShadowQuality.High,
            ShadowBias = 0.0,
            ShadowCascadeCount = 4,
            ShadowCascadeResolutions = [4096, 2048, 2048, 1024],
            SupportSoftShadows = true,
            CloudsResolutionMultiplier = 1.0,
            SkyResolution = 512,
            VegetationInstanceBudget = 65536,
            LodBias = -1,
            MaxGiProbeStatesPerSnapshot = 4
        },
        High = new GraphicsQualityPresetSettings
        {
            ResolutionFactor = 1.0,
            FpsCap = 120,
            MsaaSamples = 4,
            ShadowQuality = GraphicsShadowQuality.High,
            ShadowBias = 0.0,
            ShadowCascadeCount = 4,
            ShadowCascadeResolutions = [2048, 2048, 1024, 1024],
            SupportSoftShadows = true,
            CloudsResolutionMultiplier = 0.75,
            SkyResolution = 256,
            VegetationInstanceBudget = 32768,
            LodBias = 0,
            MaxGiProbeStatesPerSnapshot = 3
        },
        Medium = new GraphicsQualityPresetSettings
        {
            ResolutionFactor = 0.85,
            FpsCap = 120,
            MsaaSamples = 2,
            ShadowQuality = GraphicsShadowQuality.Medium,
            ShadowBias = 0.0,
            ShadowCascadeCount = 3,
            ShadowCascadeResolutions = [2048, 1024, 512],
            SupportSoftShadows = true,
            CloudsResolutionMultiplier = 0.5,
            SkyResolution = 256,
            VegetationInstanceBudget = 16384,
            LodBias = 0,
            MaxGiProbeStatesPerSnapshot = 2
        },
        Low = new GraphicsQualityPresetSettings
        {
            ResolutionFactor = 0.7,
            FpsCap = 120,
            MsaaSamples = 1,
            ShadowQuality = GraphicsShadowQuality.Low,
            ShadowBias = 0.0,
            ShadowCascadeCount = 2,
            ShadowCascadeResolutions = [1024, 512],
            SupportSoftShadows = false,
            CloudsResolutionMultiplier = 0.25,
            SkyResolution = 128,
            VegetationInstanceBudget = 8192,
            LodBias = 1,
            MaxGiProbeStatesPerSnapshot = 2
        },
        VeryLow = new GraphicsQualityPresetSettings
        {
            ResolutionFactor = 0.5,
            FpsCap = 120,
            MsaaSamples = 1,
            ShadowQuality = GraphicsShadowQuality.VeryLow,
            ShadowBias = 0.0,
            ShadowCascadeCount = 1,
            ShadowCascadeResolutions = [512],
            SupportSoftShadows = false,
            CloudsResolutionMultiplier = 0.125,
            SkyResolution = 64,
            VegetationInstanceBudget = 2048,
            LodBias = 2,
            MaxGiProbeStatesPerSnapshot = 1
        }
    };

    public static ProjectSettingsDocument Project { get; } = new()
    {
        SettingsVersion = ProjectSettingsDocument.CurrentVersion,
        Graphics = new ProjectGraphicsSettings
        {
            DefaultQuality = GraphicsQualityLevel.High,
            Presets = Presets
        }
    };

    public static WorkspaceEditorSettingsDocument Editor { get; } = new()
    {
        SettingsVersion = WorkspaceEditorSettingsDocument.CurrentVersion,
        Graphics = new EditorGraphicsSettings
        {
            SelectedQuality = EditorQualitySelection.ProjectDefault,
            StatsMode = GraphicsStatsMode.None
        }
    };
}

public sealed record GraphicsSettingsValidationIssue(string Path, string Message);

public sealed record GraphicsSettingsValidationResult(
    IReadOnlyList<GraphicsSettingsValidationIssue> Issues)
{
    public static GraphicsSettingsValidationResult Success { get; } = new([]);

    public bool IsValid => Issues.Count == 0;
}

public static class GraphicsSettingsValidator
{
    static readonly int[] SupportedMsaaSamples = [1, 2, 4, 8];

    public static GraphicsSettingsValidationResult Validate(
        ProjectSettingsDocument? document)
    {
        var issues = new List<GraphicsSettingsValidationIssue>();
        if (document is null)
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                "ProjectSettings",
                "Project settings document is required."));
            return new GraphicsSettingsValidationResult(issues);
        }

        if (document.SettingsVersion != ProjectSettingsDocument.CurrentVersion)
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                "settingsVersion",
                $"Unsupported project settings version '{document.SettingsVersion}'. Supported version is '{ProjectSettingsDocument.CurrentVersion}'."));
        }

        if (!Enum.IsDefined(document.Graphics.DefaultQuality))
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                "graphics.defaultQuality",
                "Default quality must be one of Ultra, High, Medium, Low, or VeryLow."));
        }

        foreach (var quality in Enum.GetValues<GraphicsQualityLevel>())
        {
            ValidatePreset(
                document.Graphics.Presets.Get(quality),
                $"graphics.presets.{quality}",
                issues);
        }

        return issues.Count == 0
            ? GraphicsSettingsValidationResult.Success
            : new GraphicsSettingsValidationResult(issues);
    }

    public static GraphicsSettingsValidationResult Validate(
        WorkspaceEditorSettingsDocument? document)
    {
        var issues = new List<GraphicsSettingsValidationIssue>();
        if (document is null)
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                "EditorSettings",
                "Editor settings document is required."));
            return new GraphicsSettingsValidationResult(issues);
        }

        if (document.SettingsVersion != WorkspaceEditorSettingsDocument.CurrentVersion)
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                "settingsVersion",
                $"Unsupported editor settings version '{document.SettingsVersion}'. Supported version is '{WorkspaceEditorSettingsDocument.CurrentVersion}'."));
        }

        if (!Enum.IsDefined(document.Graphics.SelectedQuality))
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                "graphics.selectedQuality",
                "Selected quality must be ProjectDefault, Ultra, High, Medium, Low, or VeryLow."));
        }

        if (!Enum.IsDefined(document.Graphics.StatsMode))
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                "graphics.statsMode",
                "Stats mode must be None, RenderStats, or RenderStatsAndQueries."));
        }

        return issues.Count == 0
            ? GraphicsSettingsValidationResult.Success
            : new GraphicsSettingsValidationResult(issues);
    }

    static void ValidatePreset(
        GraphicsQualityPresetSettings? preset,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        if (preset is null)
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                path,
                "The fixed quality preset is required."));
            return;
        }

        if (!double.IsFinite(preset.ResolutionFactor) ||
            preset.ResolutionFactor is < 0.25 or > 2.0)
        {
            AddRangeIssue(issues, $"{path}.resolutionFactor", "Resolution factor", "0.25 and 2.0");
        }

        if (preset.FpsCap is < 1 or > 1000)
        {
            AddRangeIssue(issues, $"{path}.fpsCap", "FPS cap", "1 and 1000");
        }

        if (!SupportedMsaaSamples.Contains(preset.MsaaSamples))
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                $"{path}.msaaSamples",
                "MSAA samples must be one of 1, 2, 4, or 8."));
        }

        if (!Enum.IsDefined(preset.ShadowQuality))
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                $"{path}.shadowQuality",
                "Shadow quality must be High, Medium, Low, or VeryLow."));
        }

        if (!double.IsFinite(preset.ShadowBias) ||
            preset.ShadowBias is < -16.0 or > 16.0)
        {
            AddRangeIssue(issues, $"{path}.shadowBias", "Shadow bias", "-16 and 16");
        }

        if (preset.ShadowCascadeCount is < 1 or > 4)
        {
            AddRangeIssue(issues, $"{path}.shadowCascadeCount", "Shadow cascade count", "1 and 4");
        }

        if (preset.ShadowCascadeResolutions is null ||
            preset.ShadowCascadeResolutions.Count != preset.ShadowCascadeCount)
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                $"{path}.shadowCascadeResolutions",
                "The number of cascade resolutions must exactly match shadowCascadeCount."));
        }
        else
        {
            for (var index = 0; index < preset.ShadowCascadeResolutions.Count; ++index)
            {
                if (!IsPowerOfTwoInRange(preset.ShadowCascadeResolutions[index]))
                {
                    issues.Add(new GraphicsSettingsValidationIssue(
                        $"{path}.shadowCascadeResolutions[{index}]",
                        "Cascade resolution must be a power of two between 32 and 8192."));
                }
            }
        }

        if (!double.IsFinite(preset.CloudsResolutionMultiplier) ||
            preset.CloudsResolutionMultiplier is < 0.0625 or > 2.0)
        {
            AddRangeIssue(issues, $"{path}.cloudsResolutionMultiplier", "Clouds resolution multiplier", "0.0625 and 2.0");
        }

        if (!IsPowerOfTwoInRange(preset.SkyResolution))
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                $"{path}.skyResolution",
                "Sky resolution must be a power of two between 32 and 8192."));
        }

        if (preset.VegetationInstanceBudget is < 0 or > 1048576)
        {
            AddRangeIssue(
                issues,
                $"{path}.vegetationInstanceBudget",
                "Vegetation instance budget",
                "0 and 1048576");
        }

        if (preset.LodBias is < -8 or > 8)
        {
            AddRangeIssue(issues, $"{path}.lodBias", "LOD bias", "-8 and 8");
        }

        if (preset.MaxGiProbeStatesPerSnapshot is < 0 or > 16)
        {
            AddRangeIssue(
                issues,
                $"{path}.maxGiProbeStatesPerSnapshot",
                "Maximum GI probe states per snapshot",
                "0 and 16");
        }
    }

    static bool IsPowerOfTwoInRange(int value)
        => value is >= 32 and <= 8192 && (value & (value - 1)) == 0;

    static void AddRangeIssue(
        ICollection<GraphicsSettingsValidationIssue> issues,
        string path,
        string displayName,
        string range)
        => issues.Add(new GraphicsSettingsValidationIssue(
            path,
            $"{displayName} must be between {range}."));
}

public sealed record GraphicsSettingsPaths(
    string WorkspaceRoot,
    string CacheDirectory)
{
    public const string ProjectFileName = "ProjectSettings.yaml";
    public const string EditorFileName = "EditorSettings.yaml";

    public string ProjectSettingsPath => Path.Combine(WorkspaceRoot, ProjectFileName);
    public string EditorSettingsPath => Path.Combine(CacheDirectory, EditorFileName);

    public static GraphicsSettingsPaths Create(
        string workspaceRoot,
        string cacheDirectory)
    {
        if (string.IsNullOrWhiteSpace(workspaceRoot))
            throw new ArgumentException("Workspace root is required.", nameof(workspaceRoot));
        if (string.IsNullOrWhiteSpace(cacheDirectory))
            throw new ArgumentException("Cache directory is required.", nameof(cacheDirectory));

        return new GraphicsSettingsPaths(
            NormalizeDirectory(workspaceRoot),
            NormalizeDirectory(cacheDirectory));
    }

    static string NormalizeDirectory(string path)
    {
        var fullPath = Path.GetFullPath(path);
        var root = Path.GetPathRoot(fullPath);
        return string.Equals(fullPath, root, PathComparison)
            ? fullPath
            : fullPath.TrimEnd(
                Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar);
    }

    static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}

public enum GraphicsSettingsFileRevisionKind
{
    Missing,
    Readable,
    Unreadable,
}

public readonly record struct GraphicsSettingsFileRevision(
    GraphicsSettingsFileRevisionKind Kind,
    string ContentHash)
{
    public static GraphicsSettingsFileRevision Missing { get; } = new(
        GraphicsSettingsFileRevisionKind.Missing,
        string.Empty);

    public static GraphicsSettingsFileRevision Unreadable { get; } = new(
        GraphicsSettingsFileRevisionKind.Unreadable,
        string.Empty);

    public static GraphicsSettingsFileRevision Readable(string contentHash)
    {
        if (string.IsNullOrWhiteSpace(contentHash))
            throw new ArgumentException(
                "A readable settings revision requires a content hash.",
                nameof(contentHash));
        return new GraphicsSettingsFileRevision(
            GraphicsSettingsFileRevisionKind.Readable,
            contentHash);
    }
}

public sealed record GraphicsSettingsSnapshot(
    GraphicsSettingsPaths Paths,
    long WorkspaceGeneration,
    ProjectSettingsDocument Project,
    WorkspaceEditorSettingsDocument Editor,
    GraphicsSettingsFileRevision ProjectRevision,
    GraphicsSettingsFileRevision EditorRevision,
    IReadOnlyList<string> Diagnostics)
{
    public GraphicsQualityLevel EffectiveQuality =>
        Editor.Graphics.SelectedQuality switch
        {
            EditorQualitySelection.ProjectDefault => Project.Graphics.DefaultQuality,
            EditorQualitySelection.Ultra => GraphicsQualityLevel.Ultra,
            EditorQualitySelection.High => GraphicsQualityLevel.High,
            EditorQualitySelection.Medium => GraphicsQualityLevel.Medium,
            EditorQualitySelection.Low => GraphicsQualityLevel.Low,
            EditorQualitySelection.VeryLow => GraphicsQualityLevel.VeryLow,
            _ => Project.Graphics.DefaultQuality
        };
}

public static class GraphicsSettingsYamlCodec
{
    static readonly GraphicsQualityLevel[] QualityLevels =
        Enum.GetValues<GraphicsQualityLevel>();

    public static string SerializeProject(ProjectSettingsDocument document)
    {
        ThrowIfInvalid(GraphicsSettingsValidator.Validate(document));
        var root = new YamlMappingNode();
        PatchProject(root, document);
        return Save(root);
    }

    public static string SerializeEditor(WorkspaceEditorSettingsDocument document)
    {
        ThrowIfInvalid(GraphicsSettingsValidator.Validate(document));
        var root = new YamlMappingNode();
        PatchEditor(root, document);
        return Save(root);
    }

    internal static ProjectSettingsDocument ParseProject(
        string yaml,
        ICollection<string> diagnostics,
        string source)
    {
        if (!TryLoadRoot(yaml, source, diagnostics, out var root))
            return GraphicsSettingsDefaults.Project;

        var issues = new List<GraphicsSettingsValidationIssue>();
        var version = ReadInt(root, "settingsVersion", "settingsVersion", issues);
        var legacyVersion = version == ProjectSettingsDocument.LegacyVersion;
        var graphics = ReadMapping(root, "graphics", "graphics", issues);
        var defaultQuality = ReadEnum<GraphicsQualityLevel>(
            graphics,
            "defaultQuality",
            "graphics.defaultQuality",
            issues);
        var presetsNode = ReadMapping(
            graphics,
            "presets",
            "graphics.presets",
            issues);
        var presets = GraphicsSettingsDefaults.Presets;
        foreach (var quality in QualityLevels)
        {
            var preset = ReadPreset(
                presetsNode,
                quality.ToString(),
                $"graphics.presets.{quality}",
                GraphicsSettingsDefaults.Presets.Get(quality),
                legacyVersion,
                issues);
            presets = presets.With(quality, preset);
        }

        var parsed = new ProjectSettingsDocument
        {
            SettingsVersion = legacyVersion
                ? ProjectSettingsDocument.CurrentVersion
                : version,
            Graphics = new ProjectGraphicsSettings
            {
                DefaultQuality = defaultQuality,
                Presets = presets
            }
        };
        issues.AddRange(GraphicsSettingsValidator.Validate(parsed).Issues);
        if (issues.Count == 0)
        {
            if (legacyVersion)
            {
                diagnostics.Add(
                    $"{source}: migrated settingsVersion 1 to 2 using the default GI probe-state budgets.");
            }
            return parsed;
        }

        AddDiagnostics(diagnostics, source, issues);
        return GraphicsSettingsDefaults.Project;
    }

    internal static WorkspaceEditorSettingsDocument ParseEditor(
        string yaml,
        ICollection<string> diagnostics,
        string source)
    {
        if (!TryLoadRoot(yaml, source, diagnostics, out var root))
            return GraphicsSettingsDefaults.Editor;

        var issues = new List<GraphicsSettingsValidationIssue>();
        var version = ReadInt(root, "settingsVersion", "settingsVersion", issues);
        var graphics = ReadMapping(root, "graphics", "graphics", issues);
        var parsed = new WorkspaceEditorSettingsDocument
        {
            SettingsVersion = version,
            Graphics = new EditorGraphicsSettings
            {
                SelectedQuality = ReadEnum<EditorQualitySelection>(
                    graphics,
                    "selectedQuality",
                    "graphics.selectedQuality",
                    issues),
                StatsMode = ReadEnum<GraphicsStatsMode>(
                    graphics,
                    "statsMode",
                    "graphics.statsMode",
                    issues)
            }
        };
        issues.AddRange(GraphicsSettingsValidator.Validate(parsed).Issues);
        if (issues.Count == 0)
            return parsed;

        AddDiagnostics(diagnostics, source, issues);
        return GraphicsSettingsDefaults.Editor;
    }

    internal static void PatchProject(
        YamlMappingNode root,
        ProjectSettingsDocument document)
    {
        SetScalar(root, "settingsVersion", document.SettingsVersion);
        var graphics = GetOrCreateMapping(root, "graphics");
        SetScalar(graphics, "defaultQuality", document.Graphics.DefaultQuality);
        var presets = GetOrCreateMapping(graphics, "presets");
        foreach (var quality in QualityLevels)
        {
            var preset = GetOrCreateMapping(presets, quality.ToString());
            PatchPreset(preset, document.Graphics.Presets.Get(quality));
        }
    }

    internal static void PatchEditor(
        YamlMappingNode root,
        WorkspaceEditorSettingsDocument document)
    {
        SetScalar(root, "settingsVersion", document.SettingsVersion);
        var graphics = GetOrCreateMapping(root, "graphics");
        SetScalar(graphics, "selectedQuality", document.Graphics.SelectedQuality);
        SetScalar(graphics, "statsMode", document.Graphics.StatsMode);
    }

    internal static bool TryLoadRoot(
        string yaml,
        string source,
        ICollection<string> diagnostics,
        out YamlMappingNode root)
    {
        try
        {
            var stream = new YamlStream();
            using var reader = new StringReader(yaml);
            stream.Load(reader);
            if (stream.Documents.Count != 1 ||
                stream.Documents[0].RootNode is not YamlMappingNode mapping)
            {
                diagnostics.Add($"{source}: expected one YAML mapping document; using built-in defaults.");
                root = new YamlMappingNode();
                return false;
            }

            root = mapping;
            return true;
        }
        catch (Exception exception)
        {
            diagnostics.Add($"{source}: failed to parse YAML ({exception.Message}); using built-in defaults.");
            root = new YamlMappingNode();
            return false;
        }
    }

    internal static string Save(YamlMappingNode root)
    {
        using var writer = new StringWriter(CultureInfo.InvariantCulture);
        new YamlStream(new YamlDocument(root)).Save(writer, false);
        return writer.ToString();
    }

    static GraphicsQualityPresetSettings ReadPreset(
        YamlMappingNode parent,
        string key,
        string path,
        GraphicsQualityPresetSettings defaults,
        bool allowMissingGiProbeStateBudget,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        var preset = ReadMapping(parent, key, path, issues);
        return new GraphicsQualityPresetSettings
        {
            ResolutionFactor = ReadDouble(preset, "resolutionFactor", $"{path}.resolutionFactor", issues),
            FpsCap = ReadInt(preset, "fpsCap", $"{path}.fpsCap", issues),
            MsaaSamples = ReadInt(preset, "msaaSamples", $"{path}.msaaSamples", issues),
            ShadowQuality = ReadEnum<GraphicsShadowQuality>(preset, "shadowQuality", $"{path}.shadowQuality", issues),
            ShadowBias = ReadDouble(preset, "shadowBias", $"{path}.shadowBias", issues),
            ShadowCascadeCount = ReadInt(preset, "shadowCascadeCount", $"{path}.shadowCascadeCount", issues),
            ShadowCascadeResolutions = ReadIntSequence(preset, "shadowCascadeResolutions", $"{path}.shadowCascadeResolutions", issues),
            SupportSoftShadows = ReadBool(preset, "supportSoftShadows", $"{path}.supportSoftShadows", issues),
            CloudsResolutionMultiplier = ReadDouble(preset, "cloudsResolutionMultiplier", $"{path}.cloudsResolutionMultiplier", issues),
            SkyResolution = ReadInt(preset, "skyResolution", $"{path}.skyResolution", issues),
            VegetationInstanceBudget = ReadOptionalInt(
                preset,
                "vegetationInstanceBudget",
                $"{path}.vegetationInstanceBudget",
                defaults.VegetationInstanceBudget,
                issues),
            LodBias = ReadInt(preset, "lodBias", $"{path}.lodBias", issues),
            MaxGiProbeStatesPerSnapshot = allowMissingGiProbeStateBudget
                ? ReadOptionalInt(
                    preset,
                    "maxGiProbeStatesPerSnapshot",
                    $"{path}.maxGiProbeStatesPerSnapshot",
                    defaults.MaxGiProbeStatesPerSnapshot,
                    issues)
                : ReadInt(
                    preset,
                    "maxGiProbeStatesPerSnapshot",
                    $"{path}.maxGiProbeStatesPerSnapshot",
                    issues)
        };
    }

    static void PatchPreset(
        YamlMappingNode preset,
        GraphicsQualityPresetSettings settings)
    {
        SetScalar(preset, "resolutionFactor", settings.ResolutionFactor);
        SetScalar(preset, "fpsCap", settings.FpsCap);
        SetScalar(preset, "msaaSamples", settings.MsaaSamples);
        SetScalar(preset, "shadowQuality", settings.ShadowQuality);
        SetScalar(preset, "shadowBias", settings.ShadowBias);
        SetScalar(preset, "shadowCascadeCount", settings.ShadowCascadeCount);
        SetNode(
            preset,
            "shadowCascadeResolutions",
            new YamlSequenceNode(settings.ShadowCascadeResolutions.Select(x =>
                new YamlScalarNode(x.ToString(CultureInfo.InvariantCulture)))));
        SetScalar(preset, "supportSoftShadows", settings.SupportSoftShadows);
        SetScalar(preset, "cloudsResolutionMultiplier", settings.CloudsResolutionMultiplier);
        SetScalar(preset, "skyResolution", settings.SkyResolution);
        SetScalar(preset, "vegetationInstanceBudget", settings.VegetationInstanceBudget);
        SetScalar(preset, "lodBias", settings.LodBias);
        SetScalar(
            preset,
            "maxGiProbeStatesPerSnapshot",
            settings.MaxGiProbeStatesPerSnapshot);
    }

    static YamlMappingNode ReadMapping(
        YamlMappingNode? parent,
        string key,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        if (parent is not null &&
            parent.Children.TryGetValue(new YamlScalarNode(key), out var node) &&
            node is YamlMappingNode mapping)
        {
            return mapping;
        }

        issues.Add(new GraphicsSettingsValidationIssue(path, "A YAML mapping is required."));
        return new YamlMappingNode();
    }

    static string? ReadScalar(
        YamlMappingNode? parent,
        string key,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        if (parent is not null &&
            parent.Children.TryGetValue(new YamlScalarNode(key), out var node) &&
            node is YamlScalarNode scalar &&
            scalar.Value is not null)
        {
            return scalar.Value;
        }

        issues.Add(new GraphicsSettingsValidationIssue(path, "A scalar value is required."));
        return null;
    }

    static int ReadInt(
        YamlMappingNode? parent,
        string key,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        var value = ReadScalar(parent, key, path, issues);
        if (value is not null &&
            int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
        {
            return parsed;
        }

        if (value is not null)
            issues.Add(new GraphicsSettingsValidationIssue(path, "An integer value is required."));
        return 0;
    }

    static int ReadOptionalInt(
        YamlMappingNode? parent,
        string key,
        string path,
        int defaultValue,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        if (parent is null ||
            !parent.Children.ContainsKey(new YamlScalarNode(key)))
        {
            return defaultValue;
        }
        return ReadInt(parent, key, path, issues);
    }

    static double ReadDouble(
        YamlMappingNode? parent,
        string key,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        var value = ReadScalar(parent, key, path, issues);
        if (value is not null &&
            double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed))
        {
            return parsed;
        }

        if (value is not null)
            issues.Add(new GraphicsSettingsValidationIssue(path, "A finite number is required."));
        return 0;
    }

    static bool ReadBool(
        YamlMappingNode? parent,
        string key,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        var value = ReadScalar(parent, key, path, issues);
        if (value is not null && bool.TryParse(value, out var parsed))
            return parsed;

        if (value is not null)
            issues.Add(new GraphicsSettingsValidationIssue(path, "A boolean value is required."));
        return false;
    }

    static T ReadEnum<T>(
        YamlMappingNode? parent,
        string key,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
        where T : struct, Enum
    {
        var value = ReadScalar(parent, key, path, issues);
        if (value is not null &&
            Enum.TryParse<T>(value, ignoreCase: false, out var parsed) &&
            Enum.IsDefined(parsed))
        {
            return parsed;
        }

        if (value is not null)
            issues.Add(new GraphicsSettingsValidationIssue(path, $"'{value}' is not a supported value."));
        return default;
    }

    static IReadOnlyList<int> ReadIntSequence(
        YamlMappingNode? parent,
        string key,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        if (parent is null ||
            !parent.Children.TryGetValue(new YamlScalarNode(key), out var node) ||
            node is not YamlSequenceNode sequence)
        {
            issues.Add(new GraphicsSettingsValidationIssue(path, "A sequence of integer values is required."));
            return [];
        }

        var values = new List<int>();
        for (var index = 0; index < sequence.Children.Count; ++index)
        {
            if (sequence.Children[index] is YamlScalarNode scalar &&
                int.TryParse(scalar.Value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed))
            {
                values.Add(parsed);
            }
            else
            {
                issues.Add(new GraphicsSettingsValidationIssue(
                    $"{path}[{index}]",
                    "An integer value is required."));
            }
        }

        return values;
    }

    static YamlMappingNode GetOrCreateMapping(
        YamlMappingNode parent,
        string key)
    {
        var yamlKey = new YamlScalarNode(key);
        if (parent.Children.TryGetValue(yamlKey, out var existing) &&
            existing is YamlMappingNode mapping)
        {
            return mapping;
        }

        var created = new YamlMappingNode();
        parent.Children[yamlKey] = created;
        return created;
    }

    static void SetScalar<T>(YamlMappingNode parent, string key, T value)
    {
        var text = value switch
        {
            bool boolean => boolean ? "true" : "false",
            double number => number.ToString("0.####", CultureInfo.InvariantCulture),
            float number => number.ToString("0.####", CultureInfo.InvariantCulture),
            IFormattable formattable => formattable.ToString(null, CultureInfo.InvariantCulture),
            _ => value?.ToString() ?? string.Empty
        };
        SetNode(parent, key, new YamlScalarNode(text));
    }

    static void SetNode(YamlMappingNode parent, string key, YamlNode value)
        => parent.Children[new YamlScalarNode(key)] = value;

    static void AddDiagnostics(
        ICollection<string> diagnostics,
        string source,
        IEnumerable<GraphicsSettingsValidationIssue> issues)
    {
        foreach (var issue in issues.Distinct())
            diagnostics.Add($"{source}: {issue.Path}: {issue.Message} Using built-in defaults.");
    }

    static void ThrowIfInvalid(GraphicsSettingsValidationResult validation)
    {
        if (validation.IsValid)
            return;

        throw new InvalidOperationException(string.Join(
            Environment.NewLine,
            validation.Issues.Select(issue => $"{issue.Path}: {issue.Message}")));
    }
}
