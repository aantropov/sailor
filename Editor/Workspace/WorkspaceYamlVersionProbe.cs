using System.Globalization;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;

namespace SailorEditor.Workspace;

public enum WorkspaceYamlVersionProbeFailure
{
    None,
    InvalidYaml,
    InvalidDocument,
    MissingVersion,
    DuplicateVersion,
    NonScalarVersion,
    InvalidVersion,
    UnsupportedVersion
}

public sealed record WorkspaceYamlVersionProbeResult(
    int? Version,
    string? Error,
    WorkspaceYamlVersionProbeFailure Failure)
{
    public bool Succeeded =>
        Version.HasValue &&
        Error is null &&
        Failure == WorkspaceYamlVersionProbeFailure.None;

    public bool IsVersionContractFailure => Failure is
        WorkspaceYamlVersionProbeFailure.MissingVersion or
        WorkspaceYamlVersionProbeFailure.DuplicateVersion or
        WorkspaceYamlVersionProbeFailure.NonScalarVersion or
        WorkspaceYamlVersionProbeFailure.InvalidVersion or
        WorkspaceYamlVersionProbeFailure.UnsupportedVersion;
}

public static class WorkspaceYamlVersionProbe
{
    public static WorkspaceYamlVersionProbeResult Probe(
        string yaml,
        string versionField,
        int supportedVersion,
        string documentName)
    {
        ArgumentNullException.ThrowIfNull(yaml);
        ArgumentException.ThrowIfNullOrWhiteSpace(versionField);
        ArgumentException.ThrowIfNullOrWhiteSpace(documentName);
        if (supportedVersion <= 0)
            throw new ArgumentOutOfRangeException(nameof(supportedVersion), "Supported YAML versions must be positive.");

        try
        {
            using var reader = new StringReader(yaml);
            var parser = new Parser(reader);
            parser.Consume<StreamStart>();
            if (!parser.TryConsume<DocumentStart>(out _))
                return InvalidDocument(documentName);
            if (!parser.TryConsume<MappingStart>(out _))
                return InvalidDocument(documentName);

            var versionOccurrences = 0;
            var versionWasNonScalar = false;
            string? rawVersion = null;
            while (!parser.Accept<MappingEnd>(out _))
            {
                if (!parser.TryConsume<Scalar>(out var key))
                {
                    parser.SkipThisAndNestedEvents();
                    parser.SkipThisAndNestedEvents();
                    continue;
                }

                if (!string.Equals(key.Value, versionField, StringComparison.Ordinal))
                {
                    parser.SkipThisAndNestedEvents();
                    continue;
                }

                versionOccurrences++;
                if (parser.TryConsume<Scalar>(out var value))
                {
                    rawVersion ??= value.Value;
                }
                else
                {
                    versionWasNonScalar = true;
                    parser.SkipThisAndNestedEvents();
                }
            }

            parser.Consume<MappingEnd>();
            parser.Consume<DocumentEnd>();
            if (!parser.TryConsume<StreamEnd>(out _))
            {
                return new WorkspaceYamlVersionProbeResult(
                    null,
                    $"{documentName} must contain exactly one YAML document.",
                    WorkspaceYamlVersionProbeFailure.InvalidDocument);
            }

            if (versionOccurrences == 0)
            {
                return new WorkspaceYamlVersionProbeResult(
                    null,
                    $"{documentName} field '{versionField}' is required.",
                    WorkspaceYamlVersionProbeFailure.MissingVersion);
            }
            if (versionOccurrences > 1)
            {
                return new WorkspaceYamlVersionProbeResult(
                    null,
                    $"{documentName} contains duplicate field '{versionField}'.",
                    WorkspaceYamlVersionProbeFailure.DuplicateVersion);
            }
            if (versionWasNonScalar)
            {
                return new WorkspaceYamlVersionProbeResult(
                    null,
                    $"{documentName} field '{versionField}' must be a scalar integer.",
                    WorkspaceYamlVersionProbeFailure.NonScalarVersion);
            }
            if (!int.TryParse(rawVersion, NumberStyles.Integer, CultureInfo.InvariantCulture, out var version))
            {
                return new WorkspaceYamlVersionProbeResult(
                    null,
                    $"{documentName} field '{versionField}' must be a positive integer.",
                    WorkspaceYamlVersionProbeFailure.InvalidVersion);
            }
            if (version <= 0)
            {
                return new WorkspaceYamlVersionProbeResult(
                    version,
                    $"{documentName} field '{versionField}' must be greater than zero.",
                    WorkspaceYamlVersionProbeFailure.InvalidVersion);
            }
            if (version != supportedVersion)
            {
                return new WorkspaceYamlVersionProbeResult(
                    version,
                    $"Unsupported {documentName.ToLowerInvariant()} version '{version}'. Supported version is '{supportedVersion}'.",
                    WorkspaceYamlVersionProbeFailure.UnsupportedVersion);
            }

            return new WorkspaceYamlVersionProbeResult(
                version,
                null,
                WorkspaceYamlVersionProbeFailure.None);
        }
        catch (YamlException ex)
        {
            return new WorkspaceYamlVersionProbeResult(
                null,
                $"{documentName} is invalid YAML: {ex.Message}",
                WorkspaceYamlVersionProbeFailure.InvalidYaml);
        }
    }

    static WorkspaceYamlVersionProbeResult InvalidDocument(string documentName)
        => new(
            null,
            $"{documentName} must contain a top-level YAML mapping.",
            WorkspaceYamlVersionProbeFailure.InvalidDocument);
}
