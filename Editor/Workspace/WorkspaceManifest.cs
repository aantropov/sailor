using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.Workspace;

public sealed record WorkspaceManifest
{
    public const int CurrentVersion = 1;

    public int ManifestVersion { get; init; }
    public string WorkspaceId { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string EnginePath { get; init; } = string.Empty;
    public string ContentPath { get; init; } = string.Empty;
    public string SourcePath { get; init; } = string.Empty;
    public string GeneratedProjectPath { get; init; } = string.Empty;
    public string CachePath { get; init; } = string.Empty;
    public string EngineReferenceKind { get; init; } = WorkspaceEngineReferenceKinds.Source;
    public string BuildPath { get; init; } = "Cache/Build";
    public string LogicOutputPath { get; init; } = "Binaries";
    public string LogicModuleName { get; init; } = "SailorGame";

    public static WorkspaceManifest CreateDefault(
        string name,
        string enginePath,
        string? workspaceId = null,
        string engineReferenceKind = WorkspaceEngineReferenceKinds.Source) => new()
    {
        ManifestVersion = CurrentVersion,
        WorkspaceId = string.IsNullOrWhiteSpace(workspaceId) ? Guid.NewGuid().ToString("D") : workspaceId.Trim(),
        Name = name.Trim(),
        EnginePath = WorkspaceManifestPaths.Normalize(enginePath),
        ContentPath = "Content",
        SourcePath = "Source",
        GeneratedProjectPath = "Generated",
        CachePath = "Cache",
        EngineReferenceKind = WorkspaceEngineReferenceKinds.Normalize(engineReferenceKind),
        BuildPath = "Cache/Build",
        LogicOutputPath = "Binaries",
        LogicModuleName = "SailorGame"
    };
}

public static class WorkspaceEngineReferenceKinds
{
    public const string Source = "source";
    public const string Installed = "installed";

    public static bool IsSupported(string value)
        => value is Source or Installed;

    public static string Normalize(string? value)
        => value?.Trim().ToLowerInvariant() ?? string.Empty;
}

public sealed record WorkspaceManifestIssue(string Field, string Message);

public sealed record WorkspaceManifestValidationResult(IReadOnlyList<WorkspaceManifestIssue> Issues)
{
    public static WorkspaceManifestValidationResult Success { get; } = new([]);

    public bool IsValid => Issues.Count == 0;
}

public sealed record WorkspaceManifestLoadResult(
    WorkspaceManifest? Manifest,
    WorkspaceManifestValidationResult Validation,
    string? Error = null)
{
    public bool Succeeded => Manifest is not null && Validation.IsValid && string.IsNullOrEmpty(Error);
}

public static class WorkspaceManifestPaths
{
    public static string Normalize(string? path)
    {
        if (string.IsNullOrWhiteSpace(path))
            return string.Empty;

        var normalized = path.Trim().Replace('\\', '/');
        while (normalized.StartsWith("./", StringComparison.Ordinal))
            normalized = normalized[2..];

        while (normalized.Length > 1 && normalized.EndsWith("/", StringComparison.Ordinal))
            normalized = normalized[..^1];

        return normalized;
    }
}

public sealed class WorkspaceManifestSerializer
{
    readonly ISerializer _serializer = new SerializerBuilder()
        .WithNamingConvention(CamelCaseNamingConvention.Instance)
        .ConfigureDefaultValuesHandling(DefaultValuesHandling.OmitNull)
        .Build();

    readonly IDeserializer _deserializer = new DeserializerBuilder()
        .WithNamingConvention(CamelCaseNamingConvention.Instance)
        .IgnoreUnmatchedProperties()
        .Build();

    public string Serialize(WorkspaceManifest manifest)
        => _serializer.Serialize(Normalize(manifest));

    public WorkspaceManifestLoadResult Deserialize(string yaml)
    {
        try
        {
            var manifest = _deserializer.Deserialize<WorkspaceManifest>(yaml) ?? new WorkspaceManifest();
            manifest = Normalize(manifest);
            return new WorkspaceManifestLoadResult(manifest, Validate(manifest));
        }
        catch (YamlException ex)
        {
            return new WorkspaceManifestLoadResult(null, WorkspaceManifestValidationResult.Success, $"Failed to parse workspace manifest: {ex.Message}");
        }
    }

    public async Task SaveAsync(string path, WorkspaceManifest manifest, CancellationToken cancellationToken = default)
    {
        var normalized = Normalize(manifest);
        var validation = Validate(normalized);
        if (!validation.IsValid)
            throw new InvalidOperationException($"Workspace manifest is invalid: {string.Join("; ", validation.Issues.Select(x => x.Message))}");

        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrWhiteSpace(directory))
            Directory.CreateDirectory(directory);

        await File.WriteAllTextAsync(path, Serialize(normalized), cancellationToken);
    }

    public async Task<WorkspaceManifestLoadResult> LoadAsync(string path, CancellationToken cancellationToken = default)
    {
        if (!File.Exists(path))
            return new WorkspaceManifestLoadResult(null, WorkspaceManifestValidationResult.Success, $"Workspace manifest was not found: {path}");

        var yaml = await File.ReadAllTextAsync(path, cancellationToken);
        return Deserialize(yaml);
    }

    public WorkspaceManifestValidationResult Validate(WorkspaceManifest manifest)
    {
        var issues = new List<WorkspaceManifestIssue>();

        if (manifest.ManifestVersion != WorkspaceManifest.CurrentVersion)
            issues.Add(new WorkspaceManifestIssue(nameof(WorkspaceManifest.ManifestVersion), $"Unsupported workspace manifest version '{manifest.ManifestVersion}'. Supported version is '{WorkspaceManifest.CurrentVersion}'."));

        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.WorkspaceId), manifest.WorkspaceId);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.Name), manifest.Name);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.EnginePath), manifest.EnginePath);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.ContentPath), manifest.ContentPath);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.SourcePath), manifest.SourcePath);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.GeneratedProjectPath), manifest.GeneratedProjectPath);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.CachePath), manifest.CachePath);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.EngineReferenceKind), manifest.EngineReferenceKind);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.BuildPath), manifest.BuildPath);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.LogicOutputPath), manifest.LogicOutputPath);
        AddRequiredStringIssue(issues, nameof(WorkspaceManifest.LogicModuleName), manifest.LogicModuleName);

        AddWorkspaceOwnedPathIssue(issues, nameof(WorkspaceManifest.ContentPath), manifest.ContentPath);
        AddWorkspaceOwnedPathIssue(issues, nameof(WorkspaceManifest.SourcePath), manifest.SourcePath);
        AddWorkspaceOwnedPathIssue(issues, nameof(WorkspaceManifest.GeneratedProjectPath), manifest.GeneratedProjectPath);
        AddWorkspaceOwnedPathIssue(issues, nameof(WorkspaceManifest.CachePath), manifest.CachePath);
        AddWorkspaceOwnedPathIssue(issues, nameof(WorkspaceManifest.BuildPath), manifest.BuildPath);
        AddWorkspaceOwnedPathIssue(issues, nameof(WorkspaceManifest.LogicOutputPath), manifest.LogicOutputPath);

        if (!string.IsNullOrWhiteSpace(manifest.EngineReferenceKind) &&
            !WorkspaceEngineReferenceKinds.IsSupported(manifest.EngineReferenceKind))
        {
            issues.Add(new WorkspaceManifestIssue(
                nameof(WorkspaceManifest.EngineReferenceKind),
                $"EngineReferenceKind must be '{WorkspaceEngineReferenceKinds.Source}' or '{WorkspaceEngineReferenceKinds.Installed}'."));
        }

        if (!string.IsNullOrWhiteSpace(manifest.LogicModuleName) && !IsCIdentifier(manifest.LogicModuleName))
        {
            issues.Add(new WorkspaceManifestIssue(
                nameof(WorkspaceManifest.LogicModuleName),
                "LogicModuleName must be a valid C identifier."));
        }

        return issues.Count == 0
            ? WorkspaceManifestValidationResult.Success
            : new WorkspaceManifestValidationResult(issues);
    }

    static WorkspaceManifest Normalize(WorkspaceManifest manifest) => manifest with
    {
        WorkspaceId = NormalizeText(manifest.WorkspaceId),
        Name = NormalizeText(manifest.Name),
        EnginePath = WorkspaceManifestPaths.Normalize(manifest.EnginePath),
        ContentPath = WorkspaceManifestPaths.Normalize(manifest.ContentPath),
        SourcePath = WorkspaceManifestPaths.Normalize(manifest.SourcePath),
        GeneratedProjectPath = WorkspaceManifestPaths.Normalize(manifest.GeneratedProjectPath),
        CachePath = WorkspaceManifestPaths.Normalize(manifest.CachePath),
        EngineReferenceKind = WorkspaceEngineReferenceKinds.Normalize(manifest.EngineReferenceKind),
        BuildPath = WorkspaceManifestPaths.Normalize(manifest.BuildPath),
        LogicOutputPath = WorkspaceManifestPaths.Normalize(manifest.LogicOutputPath),
        LogicModuleName = NormalizeText(manifest.LogicModuleName)
    };

    static string NormalizeText(string? value) => value?.Trim() ?? string.Empty;

    static void AddRequiredStringIssue(ICollection<WorkspaceManifestIssue> issues, string field, string value)
    {
        if (string.IsNullOrWhiteSpace(value))
            issues.Add(new WorkspaceManifestIssue(field, $"{field} is required."));
    }

    static void AddWorkspaceOwnedPathIssue(ICollection<WorkspaceManifestIssue> issues, string field, string value)
    {
        if (string.IsNullOrWhiteSpace(value) || WorkspacePathPolicy.IsSafeRelativePath(value))
            return;

        issues.Add(new WorkspaceManifestIssue(
            field,
            $"{field} must be a safe relative path inside the workspace root; rooted, drive-relative, and traversal paths can resolve outside it and are not allowed."));
    }

    static bool IsCIdentifier(string value)
    {
        if (value.Length == 0 || !IsAsciiLetterOrUnderscore(value[0]))
            return false;

        return value.Skip(1).All(x => IsAsciiLetterOrUnderscore(x) || x is >= '0' and <= '9');
    }

    static bool IsAsciiLetterOrUnderscore(char value)
        => value == '_' || value is >= 'A' and <= 'Z' || value is >= 'a' and <= 'z';
}
