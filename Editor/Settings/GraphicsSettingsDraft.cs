using System.Globalization;

namespace SailorEditor.Settings;

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
    string SkyResolution,
    string LodBias)
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
            settings.SkyResolution.ToString(CultureInfo.InvariantCulture),
            settings.LodBias.ToString(CultureInfo.InvariantCulture));

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
            LodBias,
            $"{path}.lodBias",
            "LOD bias",
            issues,
            out var lodBias);

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
            SkyResolution = skyResolution,
            LodBias = lodBias
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
