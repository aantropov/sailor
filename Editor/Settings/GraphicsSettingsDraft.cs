using System.Globalization;

namespace SailorEditor.Settings;

public sealed record RuntimeGIProbesQualityDraft(
    bool Enabled,
    string MaxActiveProbes,
    string SpacingMultiplier,
    string InitialSamplesPerProbe,
    string TargetSamplesPerProbe,
    string WorkerCount,
    string CpuDutyFraction,
    string CpuBudgetMilliseconds,
    string MaxPublicationsPerSecond,
    string InitialPublicationCoverage,
    string MaxDirtyUploadBytesPerFrame)
{
    public static RuntimeGIProbesQualityDraft FromSettings(
        RuntimeGIProbesQualitySettings settings) => new(
            settings.Enabled,
            settings.MaxActiveProbes.ToString(CultureInfo.InvariantCulture),
            settings.SpacingMultiplier.ToString("0.####", CultureInfo.InvariantCulture),
            settings.InitialSamplesPerProbe.ToString(CultureInfo.InvariantCulture),
            settings.TargetSamplesPerProbe.ToString(CultureInfo.InvariantCulture),
            settings.WorkerCount.ToString(CultureInfo.InvariantCulture),
            settings.CpuDutyFraction.ToString("0.####", CultureInfo.InvariantCulture),
            settings.CpuBudgetMilliseconds.ToString("0.####", CultureInfo.InvariantCulture),
            settings.MaxPublicationsPerSecond.ToString("0.####", CultureInfo.InvariantCulture),
            settings.InitialPublicationCoverage.ToString("0.####", CultureInfo.InvariantCulture),
            settings.MaxDirtyUploadBytesPerFrame.ToString(CultureInfo.InvariantCulture));
}

public sealed record GraphicsQualityPresetDraft(
    string ResolutionFactor,
    string FpsCap,
    int MsaaSamples,
    GraphicsShadowQuality ShadowQuality,
    string ShadowBias,
    int ShadowCascadeCount,
    string ShadowCascadeResolutions,
    bool SupportSoftShadows,
    string CloudsResolutionMultiplier,
    bool CloudsDithering,
    string SkyResolution,
    string VegetationInstanceBudget,
    string LodBias,
    bool EnableGlobalIllumination,
    string MaxGiProbeStatesPerSnapshot,
    RuntimeGIProbesQualityDraft RuntimeGIProbes)
{
    public static GraphicsQualityPresetDraft FromSettings(
        GraphicsQualityPresetSettings settings)
        => new(
            FormatDouble(settings.ResolutionFactor),
            settings.FpsCap.ToString(CultureInfo.InvariantCulture),
            settings.MsaaSamples,
            settings.ShadowQuality,
            FormatDouble(settings.ShadowBias),
            settings.ShadowCascadeCount,
            string.Join(", ", settings.ShadowCascadeResolutions),
            settings.SupportSoftShadows,
            FormatDouble(settings.CloudsResolutionMultiplier),
            settings.CloudsDithering,
            settings.SkyResolution.ToString(CultureInfo.InvariantCulture),
            settings.VegetationInstanceBudget.ToString(CultureInfo.InvariantCulture),
            settings.LodBias.ToString(CultureInfo.InvariantCulture),
            settings.EnableGlobalIllumination,
            settings.MaxGiProbeStatesPerSnapshot.ToString(
                CultureInfo.InvariantCulture),
            RuntimeGIProbesQualityDraft.FromSettings(settings.RuntimeGIProbes));

    internal bool TryBuild(
        GraphicsQualityLevel quality,
        out GraphicsQualityPresetSettings settings,
        ICollection<GraphicsSettingsValidationIssue> issues)
    {
        var path = $"graphics.presets.{quality}";
        var valid = true;
        valid &= TryParseDouble(
            ResolutionFactor,
            $"{path}.resolutionFactor",
            "Resolution factor",
            issues,
            out var resolutionFactor);
        valid &= TryParseInt(
            FpsCap,
            $"{path}.fpsCap",
            "FPS cap",
            issues,
            out var fpsCap);
        valid &= TryParseCascadeResolutions(
            ShadowCascadeResolutions,
            $"{path}.shadowCascadeResolutions",
            issues,
            out var cascadeResolutions);
        valid &= TryParseDouble(
            ShadowBias,
            $"{path}.shadowBias",
            "Shadow bias",
            issues,
            out var shadowBias);
        valid &= TryParseDouble(
            CloudsResolutionMultiplier,
            $"{path}.cloudsResolutionMultiplier",
            "Clouds resolution multiplier",
            issues,
            out var cloudsResolutionMultiplier);
        valid &= TryParseInt(
            SkyResolution,
            $"{path}.skyResolution",
            "Sky resolution",
            issues,
            out var skyResolution);
        valid &= TryParseInt(
            VegetationInstanceBudget,
            $"{path}.vegetationInstanceBudget",
            "Vegetation instance budget",
            issues,
            out var vegetationInstanceBudget);
        valid &= TryParseInt(
            LodBias,
            $"{path}.lodBias",
            "LOD bias",
            issues,
            out var lodBias);
        valid &= TryParseInt(
            MaxGiProbeStatesPerSnapshot,
            $"{path}.maxGiProbeStatesPerSnapshot",
            "Maximum GI probe states per snapshot",
            issues,
            out var maxGiProbeStatesPerSnapshot);
        var runtimePath = $"{path}.runtimeGIProbes";
        valid &= TryParseInt(RuntimeGIProbes.MaxActiveProbes, $"{runtimePath}.maxActiveProbes", "Runtime GI probe capacity", issues, out var runtimeMaxActiveProbes);
        valid &= TryParseDouble(RuntimeGIProbes.SpacingMultiplier, $"{runtimePath}.spacingMultiplier", "Runtime GI spacing multiplier", issues, out var runtimeSpacingMultiplier);
        valid &= TryParseInt(RuntimeGIProbes.InitialSamplesPerProbe, $"{runtimePath}.initialSamplesPerProbe", "Runtime GI initial samples", issues, out var runtimeInitialSamples);
        valid &= TryParseInt(RuntimeGIProbes.TargetSamplesPerProbe, $"{runtimePath}.targetSamplesPerProbe", "Runtime GI target samples", issues, out var runtimeTargetSamples);
        valid &= TryParseInt(RuntimeGIProbes.WorkerCount, $"{runtimePath}.workerCount", "Runtime GI worker count", issues, out var runtimeWorkerCount);
        valid &= TryParseDouble(RuntimeGIProbes.CpuDutyFraction, $"{runtimePath}.cpuDutyFraction", "Runtime GI CPU duty fraction", issues, out var runtimeCpuDuty);
        valid &= TryParseDouble(RuntimeGIProbes.CpuBudgetMilliseconds, $"{runtimePath}.cpuBudgetMilliseconds", "Runtime GI CPU budget", issues, out var runtimeCpuBudget);
        valid &= TryParseDouble(RuntimeGIProbes.MaxPublicationsPerSecond, $"{runtimePath}.maxPublicationsPerSecond", "Runtime GI publication rate", issues, out var runtimePublicationRate);
        valid &= TryParseDouble(RuntimeGIProbes.InitialPublicationCoverage, $"{runtimePath}.initialPublicationCoverage", "Runtime GI initial coverage", issues, out var runtimeInitialCoverage);
        valid &= TryParseInt(RuntimeGIProbes.MaxDirtyUploadBytesPerFrame, $"{runtimePath}.maxDirtyUploadBytesPerFrame", "Runtime GI upload budget", issues, out var runtimeUploadBudget);

        settings = new GraphicsQualityPresetSettings
        {
            ResolutionFactor = resolutionFactor,
            FpsCap = fpsCap,
            MsaaSamples = MsaaSamples,
            ShadowQuality = ShadowQuality,
            ShadowBias = shadowBias,
            ShadowCascadeCount = ShadowCascadeCount,
            ShadowCascadeResolutions = cascadeResolutions,
            SupportSoftShadows = SupportSoftShadows,
            CloudsResolutionMultiplier = cloudsResolutionMultiplier,
            CloudsDithering = CloudsDithering,
            SkyResolution = skyResolution,
            VegetationInstanceBudget = vegetationInstanceBudget,
            LodBias = lodBias,
            EnableGlobalIllumination = EnableGlobalIllumination,
            MaxGiProbeStatesPerSnapshot = maxGiProbeStatesPerSnapshot,
            RuntimeGIProbes = new RuntimeGIProbesQualitySettings
            {
                Enabled = RuntimeGIProbes.Enabled,
                MaxActiveProbes = runtimeMaxActiveProbes,
                SpacingMultiplier = runtimeSpacingMultiplier,
                InitialSamplesPerProbe = runtimeInitialSamples,
                TargetSamplesPerProbe = runtimeTargetSamples,
                WorkerCount = runtimeWorkerCount,
                CpuDutyFraction = runtimeCpuDuty,
                CpuBudgetMilliseconds = runtimeCpuBudget,
                MaxPublicationsPerSecond = runtimePublicationRate,
                InitialPublicationCoverage = runtimeInitialCoverage,
                MaxDirtyUploadBytesPerFrame = runtimeUploadBudget
            }
        };
        return valid;
    }

    static bool TryParseDouble(
        string? text,
        string path,
        string displayName,
        ICollection<GraphicsSettingsValidationIssue> issues,
        out double value)
    {
        if (double.TryParse(
                text,
                NumberStyles.Float,
                CultureInfo.InvariantCulture,
                out value) &&
            double.IsFinite(value))
        {
            return true;
        }

        issues.Add(new GraphicsSettingsValidationIssue(
            path,
            $"{displayName} must be a finite number using '.' as the decimal separator."));
        value = 0;
        return false;
    }

    static bool TryParseInt(
        string? text,
        string path,
        string displayName,
        ICollection<GraphicsSettingsValidationIssue> issues,
        out int value)
    {
        if (int.TryParse(
                text,
                NumberStyles.Integer,
                CultureInfo.InvariantCulture,
                out value))
        {
            return true;
        }

        issues.Add(new GraphicsSettingsValidationIssue(
            path,
            $"{displayName} must be an integer."));
        value = 0;
        return false;
    }

    static bool TryParseCascadeResolutions(
        string? text,
        string path,
        ICollection<GraphicsSettingsValidationIssue> issues,
        out IReadOnlyList<int> values)
    {
        var parsed = new List<int>();
        var parts = (text ?? string.Empty).Split(
            ',',
            StringSplitOptions.TrimEntries);
        if (parts.Length == 0 || parts.Any(string.IsNullOrWhiteSpace))
        {
            issues.Add(new GraphicsSettingsValidationIssue(
                path,
                "Cascade resolutions must be a comma-separated list of integers."));
            values = [];
            return false;
        }

        foreach (var part in parts)
        {
            if (!int.TryParse(
                    part,
                    NumberStyles.Integer,
                    CultureInfo.InvariantCulture,
                    out var value))
            {
                issues.Add(new GraphicsSettingsValidationIssue(
                    path,
                    $"Cascade resolution '{part}' is not an integer."));
                values = [];
                return false;
            }
            parsed.Add(value);
        }

        values = parsed;
        return true;
    }

    static string FormatDouble(double value)
        => value.ToString("0.####", CultureInfo.InvariantCulture);
}

public sealed class GraphicsSettingsDraft
{
    readonly Dictionary<GraphicsQualityLevel, GraphicsQualityPresetDraft> _presets;

    internal GraphicsSettingsDraft(GraphicsSettingsSnapshot sourceSnapshot)
    {
        SourceSnapshot = sourceSnapshot ??
            throw new ArgumentNullException(nameof(sourceSnapshot));
        ProjectDefaultQuality = sourceSnapshot.Project.Graphics.DefaultQuality;
        SelectedQuality = sourceSnapshot.Editor.Graphics.SelectedQuality;
        StatsMode = sourceSnapshot.Editor.Graphics.StatsMode;
        _presets = Enum.GetValues<GraphicsQualityLevel>()
            .ToDictionary(
                quality => quality,
                quality => GraphicsQualityPresetDraft.FromSettings(
                    sourceSnapshot.Project.Graphics.Presets.Get(quality)));
    }

    public GraphicsSettingsSnapshot SourceSnapshot { get; }
    public GraphicsQualityLevel ProjectDefaultQuality { get; private set; }
    public EditorQualitySelection SelectedQuality { get; private set; }
    public GraphicsStatsMode StatsMode { get; private set; }
    public bool IsDirty { get; private set; }

    public GraphicsQualityPresetDraft GetPreset(GraphicsQualityLevel quality)
        => _presets[quality];

    public void SetSelections(
        GraphicsQualityLevel projectDefaultQuality,
        EditorQualitySelection selectedQuality,
        GraphicsStatsMode statsMode)
    {
        if (ProjectDefaultQuality == projectDefaultQuality &&
            SelectedQuality == selectedQuality &&
            StatsMode == statsMode)
        {
            return;
        }

        ProjectDefaultQuality = projectDefaultQuality;
        SelectedQuality = selectedQuality;
        StatsMode = statsMode;
        IsDirty = true;
    }

    public void SetPreset(
        GraphicsQualityLevel quality,
        GraphicsQualityPresetDraft preset)
    {
        ArgumentNullException.ThrowIfNull(preset);
        if (_presets[quality] == preset)
            return;

        _presets[quality] = preset;
        IsDirty = true;
    }

    public bool TryBuild(
        out ProjectSettingsDocument project,
        out WorkspaceEditorSettingsDocument editor,
        out IReadOnlyList<GraphicsSettingsValidationIssue> issues)
    {
        var collected = new List<GraphicsSettingsValidationIssue>();
        var presets = SourceSnapshot.Project.Graphics.Presets;
        var valid = true;
        foreach (var quality in Enum.GetValues<GraphicsQualityLevel>())
        {
            valid &= _presets[quality].TryBuild(
                quality,
                out var preset,
                collected);
            presets = presets.With(quality, preset);
        }

        project = SourceSnapshot.Project with
        {
            Graphics = SourceSnapshot.Project.Graphics with
            {
                DefaultQuality = ProjectDefaultQuality,
                Presets = presets
            }
        };
        editor = SourceSnapshot.Editor with
        {
            Graphics = SourceSnapshot.Editor.Graphics with
            {
                SelectedQuality = SelectedQuality,
                StatsMode = StatsMode
            }
        };
        collected.AddRange(GraphicsSettingsValidator.Validate(project).Issues);
        collected.AddRange(GraphicsSettingsValidator.Validate(editor).Issues);
        issues = collected.Distinct().ToArray();
        return valid && issues.Count == 0;
    }
}

public sealed class GraphicsSettingsDraftSession
{
    public GraphicsSettingsDraft? Current { get; private set; }

    public GraphicsSettingsDraft GetOrCreate(GraphicsSettingsSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        if (Current is null || !BelongsToSameWorkspace(Current, snapshot))
            Current = new GraphicsSettingsDraft(snapshot);
        return Current;
    }

    public GraphicsSettingsDraft Replace(GraphicsSettingsSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        Current = new GraphicsSettingsDraft(snapshot);
        return Current;
    }

    public void Clear()
        => Current = null;

    static bool BelongsToSameWorkspace(
        GraphicsSettingsDraft draft,
        GraphicsSettingsSnapshot snapshot)
        => draft.SourceSnapshot.WorkspaceGeneration == snapshot.WorkspaceGeneration &&
            draft.SourceSnapshot.Paths == snapshot.Paths;
}
