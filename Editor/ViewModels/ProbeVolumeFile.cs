using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Numerics;

namespace SailorEditor.ViewModels;

public sealed class ProbeVolumeFile : AssetFile
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
        catch (Exception exception)
        {
            SetLoadError(exception);
        }

        ResetDirtyState();
        return Task.CompletedTask;
    }

    protected override void AddTransientAssetProperties(
        ObservableDictionary<string, ObservableObject> properties)
    {
        var metadata = ProbeVolumeBinaryMetadata.Read(Asset);
        Add(properties, "state", metadata.StateName);
        Add(properties, "formatVersion", metadata.FormatVersion.ToString());
        Add(properties, "bakedStates", metadata.BakedStateCount.ToString());
        Add(properties, "baker", metadata.BakerVersion);
        Add(properties, "sphericalHarmonicsOrder", metadata.SphericalHarmonicsOrder.ToString());
        Add(properties, "compression", metadata.Compression == 0 ? "Float32" : metadata.Compression.ToString());
        Add(properties, "boundsMin", Format(metadata.VolumeMin));
        Add(properties, "boundsMax", Format(metadata.VolumeMax));
        Add(properties, "bricks", metadata.BrickCount.ToString());
        Add(properties, "probes", metadata.ProbeCount.ToString());
        Add(properties, "invalidProbes", metadata.InvalidProbeCount.ToString());
        Add(properties, "relocatedProbes", metadata.RelocatedProbeCount.ToString());
        Add(properties, "averageValidity", metadata.AverageValidity.ToString("0.###"));
        Add(properties, "bakeDurationSeconds", metadata.BakeDurationSeconds.ToString("0.###"));
        Add(properties, "raysPerProbe", metadata.RaysPerProbe.ToString());
        Add(properties, "bounceCount", metadata.BounceCount.ToString());
        Add(properties, "randomSeed", metadata.RandomSeed.ToString());
        Add(properties, "maxSubdivisionLevel", metadata.MaxSubdivisionLevel.ToString());
        Add(properties, "minProbeSpacing", metadata.MinProbeSpacing.ToString("0.###"));
        Add(properties, "normalBias", metadata.NormalBias.ToString("0.###"));
        Add(properties, "viewBias", metadata.ViewBias.ToString("0.###"));
        Add(properties, "maxRayDistance", metadata.MaxRayDistance.ToString("0.###"));
        Add(properties, "includeSky", metadata.IncludeSky.ToString());
        Add(properties, "includeEmissive", metadata.IncludeEmissive.ToString());
        Add(properties, "includeDirectLighting", metadata.IncludeDirectLighting.ToString());
        Add(properties, "layoutHash", FormatHash(metadata.LayoutHash));
        Add(properties, "representationHash", FormatHash(metadata.RepresentationHash));
        Add(properties, "transportHash", FormatHash(metadata.TransportHash));
        Add(properties, "lightingHash", FormatHash(metadata.LightingHash));
        Add(properties, "sourceWorldHash", FormatHash(metadata.SourceWorldHash));
        Add(properties, "payloadChecksum", FormatHash(metadata.PayloadChecksum));
        Add(properties, "fileBytes", metadata.FileBytes.ToString());
        Add(properties, "diagnostic", metadata.Diagnostic);
    }

    void Add(
        ObservableDictionary<string, ObservableObject> properties,
        string name,
        string value) => PrefabWorldStats.AddReadOnlyStat(
            properties,
            ReadOnlyAssetProperties,
            TransientAssetProperties,
            name,
            value);

    static string Format(Vector3 value) =>
        $"{value.X:0.###}, {value.Y:0.###}, {value.Z:0.###}";

    static string FormatHash(ulong value) => $"0x{value:X16}";
}
