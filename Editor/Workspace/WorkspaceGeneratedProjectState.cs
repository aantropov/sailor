using System.Globalization;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.Workspace;

public enum WorkspaceGeneratedProjectStateStatus
{
    Current,
    Untracked,
    Stale,
    Invalid
}

public sealed record WorkspaceGeneratedProjectStateMismatch(
    string Field,
    string RecordedValue,
    string CurrentValue);

public sealed record WorkspaceGeneratedProjectStateAssessment(
    WorkspaceGeneratedProjectStateStatus Status,
    string Guidance,
    IReadOnlyList<WorkspaceGeneratedProjectStateMismatch> Mismatches)
{
    public bool RequiresAttention => Status != WorkspaceGeneratedProjectStateStatus.Current;
}

public sealed record WorkspaceGeneratedProjectCreationInputs
{
    public int? ManifestVersion { get; init; }
    public string? EnginePath { get; init; }
    public string? EngineReferenceKind { get; init; }
    public string? ContentPath { get; init; }
    public string? SourcePath { get; init; }
    public string? GeneratedProjectPath { get; init; }
    public string? CachePath { get; init; }
    public string? BuildPath { get; init; }
    public string? LogicOutputPath { get; init; }
    public string? LogicModuleName { get; init; }
}

public sealed record WorkspaceGeneratedProjectState
{
    public int GeneratorSchemaVersion { get; init; }
    public WorkspaceGeneratedProjectCreationInputs? CreationInputs { get; init; }
}

public sealed class WorkspaceGeneratedProjectStateService
{
    public const int CurrentGeneratorSchemaVersion = 1;
    public const string StateDirectoryName = ".sailor";
    public const string StateFileName = "GeneratedProjectState.yaml";
    public const string RelativePath = StateDirectoryName + "/" + StateFileName;

    const string VersionField = "generatorSchemaVersion";
    const string DocumentName = "Generated project state";

    static readonly IReadOnlyList<WorkspaceGeneratedProjectStateMismatch> NoMismatches =
        Array.Empty<WorkspaceGeneratedProjectStateMismatch>();

    readonly ISerializer _serializer = new SerializerBuilder()
        .WithNamingConvention(CamelCaseNamingConvention.Instance)
        .Build();

    readonly IDeserializer _deserializer = new DeserializerBuilder()
        .WithNamingConvention(CamelCaseNamingConvention.Instance)
        .IgnoreUnmatchedProperties()
        .Build();

    public string GetStatePath(string workspaceRoot)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(workspaceRoot);

        var root = WorkspacePathPolicy.NormalizePhysicalPath(workspaceRoot);
        var statePath = Path.GetFullPath(Path.Combine(root, StateDirectoryName, StateFileName));
        WorkspacePathPolicy.EnsureInsideRoot(root, statePath, RelativePath);
        return statePath;
    }

    public WorkspaceGeneratedProjectState Create(WorkspaceManifest manifest)
    {
        ArgumentNullException.ThrowIfNull(manifest);

        return new WorkspaceGeneratedProjectState
        {
            GeneratorSchemaVersion = CurrentGeneratorSchemaVersion,
            CreationInputs = BuildInputs(manifest)
        };
    }

    public string Serialize(WorkspaceManifest manifest)
    {
        var yaml = _serializer.Serialize(Create(manifest)).ReplaceLineEndings("\n");
        return yaml.EndsWith('\n') ? yaml : yaml + "\n";
    }

    public async Task<WorkspaceGeneratedProjectStateAssessment> AssessAsync(
        string workspaceRoot,
        WorkspaceManifest manifest,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(manifest);

        string statePath;
        try
        {
            statePath = GetStatePath(workspaceRoot);
        }
        catch (Exception ex) when (ex is ArgumentException or IOException or InvalidOperationException or
            NotSupportedException or UnauthorizedAccessException)
        {
            return Invalid("The generated project state path is not safely contained by the workspace root.");
        }

        string yaml;
        try
        {
            yaml = await File.ReadAllTextAsync(statePath, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex) when (ex is FileNotFoundException or DirectoryNotFoundException)
        {
            return Untracked();
        }
        catch (Exception ex) when (ex is IOException or NotSupportedException or UnauthorizedAccessException)
        {
            return Invalid($"The generated project state at '{RelativePath}' could not be read.");
        }

        var version = WorkspaceYamlVersionProbe.Probe(
            yaml,
            VersionField,
            CurrentGeneratorSchemaVersion,
            DocumentName);
        if (!version.Succeeded)
            return Invalid(version.Error ?? $"The generated project state at '{RelativePath}' is invalid.");

        WorkspaceGeneratedProjectState? state;
        try
        {
            state = _deserializer.Deserialize<WorkspaceGeneratedProjectState>(yaml);
        }
        catch (YamlException)
        {
            return Invalid($"The generated project state at '{RelativePath}' is invalid YAML.");
        }

        if (state is null ||
            state.GeneratorSchemaVersion != CurrentGeneratorSchemaVersion ||
            !HasCompleteInputs(state.CreationInputs))
        {
            return Invalid($"The generated project state at '{RelativePath}' does not contain the complete schema v{CurrentGeneratorSchemaVersion} creation inputs.");
        }

        var recorded = NormalizeInputs(state.CreationInputs!);
        if (!HasValidInputs(recorded))
        {
            return Invalid($"The generated project state at '{RelativePath}' contains malformed or unsafe creation inputs.");
        }

        var current = BuildInputs(manifest);
        var mismatches = Compare(recorded, current);
        if (mismatches.Count == 0)
        {
            return new WorkspaceGeneratedProjectStateAssessment(
                WorkspaceGeneratedProjectStateStatus.Current,
                string.Empty,
                NoMismatches);
        }

        return new WorkspaceGeneratedProjectStateAssessment(
            WorkspaceGeneratedProjectStateStatus.Stale,
            BuildStaleGuidance(mismatches),
            mismatches);
    }

    static WorkspaceGeneratedProjectCreationInputs BuildInputs(WorkspaceManifest manifest)
        => NormalizeInputs(new WorkspaceGeneratedProjectCreationInputs
        {
            ManifestVersion = manifest.ManifestVersion,
            EnginePath = manifest.EnginePath,
            EngineReferenceKind = manifest.EngineReferenceKind,
            ContentPath = manifest.ContentPath,
            SourcePath = manifest.SourcePath,
            GeneratedProjectPath = manifest.GeneratedProjectPath,
            CachePath = manifest.CachePath,
            BuildPath = manifest.BuildPath,
            LogicOutputPath = manifest.LogicOutputPath,
            LogicModuleName = manifest.LogicModuleName
        });

    static WorkspaceGeneratedProjectCreationInputs NormalizeInputs(WorkspaceGeneratedProjectCreationInputs inputs)
        => inputs with
        {
            EnginePath = WorkspaceManifestPaths.Normalize(inputs.EnginePath),
            EngineReferenceKind = WorkspaceEngineReferenceKinds.Normalize(inputs.EngineReferenceKind),
            ContentPath = WorkspaceManifestPaths.Normalize(inputs.ContentPath),
            SourcePath = WorkspaceManifestPaths.Normalize(inputs.SourcePath),
            GeneratedProjectPath = WorkspaceManifestPaths.Normalize(inputs.GeneratedProjectPath),
            CachePath = WorkspaceManifestPaths.Normalize(inputs.CachePath),
            BuildPath = WorkspaceManifestPaths.Normalize(inputs.BuildPath),
            LogicOutputPath = WorkspaceManifestPaths.Normalize(inputs.LogicOutputPath),
            LogicModuleName = inputs.LogicModuleName?.Trim() ?? string.Empty
        };

    static bool HasCompleteInputs(WorkspaceGeneratedProjectCreationInputs? inputs)
        => inputs is not null &&
            inputs.ManifestVersion.HasValue &&
            inputs.EnginePath is not null &&
            inputs.EngineReferenceKind is not null &&
            inputs.ContentPath is not null &&
            inputs.SourcePath is not null &&
            inputs.GeneratedProjectPath is not null &&
            inputs.CachePath is not null &&
            inputs.BuildPath is not null &&
            inputs.LogicOutputPath is not null &&
            inputs.LogicModuleName is not null;

    static bool HasValidInputs(WorkspaceGeneratedProjectCreationInputs inputs)
        => inputs.ManifestVersion > 0 &&
            !string.IsNullOrWhiteSpace(inputs.EnginePath) &&
            WorkspaceEngineReferenceKinds.IsSupported(inputs.EngineReferenceKind!) &&
            WorkspacePathPolicy.IsSafeRelativePath(inputs.ContentPath) &&
            WorkspacePathPolicy.IsSafeRelativePath(inputs.SourcePath) &&
            WorkspacePathPolicy.IsSafeRelativePath(inputs.GeneratedProjectPath) &&
            WorkspacePathPolicy.IsSafeRelativePath(inputs.CachePath) &&
            WorkspacePathPolicy.IsSafeRelativePath(inputs.BuildPath) &&
            WorkspacePathPolicy.IsSafeRelativePath(inputs.LogicOutputPath) &&
            IsCIdentifier(inputs.LogicModuleName!);

    static bool IsCIdentifier(string value)
    {
        if (value.Length == 0 || !IsAsciiLetterOrUnderscore(value[0]))
            return false;

        return value.Skip(1).All(x => IsAsciiLetterOrUnderscore(x) || x is >= '0' and <= '9');
    }

    static bool IsAsciiLetterOrUnderscore(char value)
        => value == '_' || value is >= 'A' and <= 'Z' || value is >= 'a' and <= 'z';

    static IReadOnlyList<WorkspaceGeneratedProjectStateMismatch> Compare(
        WorkspaceGeneratedProjectCreationInputs recorded,
        WorkspaceGeneratedProjectCreationInputs current)
    {
        var mismatches = new List<WorkspaceGeneratedProjectStateMismatch>();
        AddMismatch(
            mismatches,
            "manifestVersion",
            recorded.ManifestVersion!.Value.ToString(CultureInfo.InvariantCulture),
            current.ManifestVersion!.Value.ToString(CultureInfo.InvariantCulture));
        AddMismatch(mismatches, "enginePath", recorded.EnginePath!, current.EnginePath!);
        AddMismatch(mismatches, "engineReferenceKind", recorded.EngineReferenceKind!, current.EngineReferenceKind!);
        AddMismatch(mismatches, "contentPath", recorded.ContentPath!, current.ContentPath!);
        AddMismatch(mismatches, "sourcePath", recorded.SourcePath!, current.SourcePath!);
        AddMismatch(mismatches, "generatedProjectPath", recorded.GeneratedProjectPath!, current.GeneratedProjectPath!);
        AddMismatch(mismatches, "cachePath", recorded.CachePath!, current.CachePath!);
        AddMismatch(mismatches, "buildPath", recorded.BuildPath!, current.BuildPath!);
        AddMismatch(mismatches, "logicOutputPath", recorded.LogicOutputPath!, current.LogicOutputPath!);
        AddMismatch(mismatches, "logicModuleName", recorded.LogicModuleName!, current.LogicModuleName!);
        return mismatches;
    }

    static void AddMismatch(
        ICollection<WorkspaceGeneratedProjectStateMismatch> mismatches,
        string field,
        string recordedValue,
        string currentValue)
    {
        if (!string.Equals(recordedValue, currentValue, StringComparison.Ordinal))
        {
            mismatches.Add(new WorkspaceGeneratedProjectStateMismatch(
                field,
                recordedValue,
                currentValue));
        }
    }

    static WorkspaceGeneratedProjectStateAssessment Untracked()
        => new(
            WorkspaceGeneratedProjectStateStatus.Untracked,
            $"Generated project state is not tracked at '{RelativePath}'. The workspace remains usable, but Sailor cannot verify that its user-owned CMake and source files match the manifest. Manually reconcile those files, or create a clean workspace with the current editor and merge the user-owned changes, then add the state file to source control; Sailor did not backfill or overwrite anything.",
            NoMismatches);

    static WorkspaceGeneratedProjectStateAssessment Invalid(string reason)
        => new(
            WorkspaceGeneratedProjectStateStatus.Invalid,
            $"{reason} Restore a compatible source-controlled state file, manually reconcile the user-owned CMake and source files, or create a clean workspace with the current editor and merge the user-owned changes; Sailor did not backfill, regenerate, or overwrite anything.",
            NoMismatches);

    static string BuildStaleGuidance(IReadOnlyList<WorkspaceGeneratedProjectStateMismatch> mismatches)
    {
        var changes = string.Join(
            "; ",
            mismatches.Select(x =>
                $"{x.Field} (recorded '{FormatValue(x.RecordedValue)}', current '{FormatValue(x.CurrentValue)}')"));
        return $"Generated project state is stale: {changes}. Restore the recorded manifest values, manually reconcile the user-owned CMake and source files, or create a clean workspace with the current editor and merge the user-owned changes, then update '{RelativePath}' in source control; Sailor did not regenerate or overwrite anything.";
    }

    static string FormatValue(string value)
        => value
            .Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("'", "''", StringComparison.Ordinal)
            .Replace("\r", "\\r", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal)
            .Replace("\t", "\\t", StringComparison.Ordinal);
}
