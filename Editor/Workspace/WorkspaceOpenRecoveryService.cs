#nullable enable

namespace SailorEditor.Workspace;

public sealed record WorkspaceOpenRecoveryAssessment(
    bool PreviousOpenWasInterrupted,
    bool ModuleBinaryIsMissing,
    string ModuleBinaryPath)
{
    public bool RequiresBuild => PreviousOpenWasInterrupted || ModuleBinaryIsMissing;
}

public sealed class WorkspaceOpenRecoveryService
{
    public const string BuildConfiguration = "Release";
    const string RecoveryDirectoryName = "Editor";
    const string PendingMarkerFileName = "WorkspaceOpen.pending";

    public WorkspaceOpenRecoveryAssessment Assess(WorkspaceSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var moduleBinaryPath = GetModuleBinaryPath(session, BuildConfiguration);
        return new WorkspaceOpenRecoveryAssessment(
            File.Exists(GetPendingMarkerPath(session)),
            !File.Exists(moduleBinaryPath),
            moduleBinaryPath);
    }

    public async Task MarkPendingAsync(
        WorkspaceSession session,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(session);
        var markerPath = GetPendingMarkerPath(session);
        Directory.CreateDirectory(Path.GetDirectoryName(markerPath)!);
        var contents = $"manifest: {session.ManifestPath}{Environment.NewLine}" +
            $"startedAtUtc: {DateTimeOffset.UtcNow:O}{Environment.NewLine}";
        await File.WriteAllTextAsync(markerPath, contents, cancellationToken);
    }

    public void ClearPending(WorkspaceSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var markerPath = GetPendingMarkerPath(session);
        if (File.Exists(markerPath))
            File.Delete(markerPath);
    }

    public string GetPendingMarkerPath(WorkspaceSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        return Path.Combine(
            session.CacheDirectory,
            RecoveryDirectoryName,
            PendingMarkerFileName);
    }

    public string GetModuleBinaryPath(
        WorkspaceSession session,
        string configuration)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentException.ThrowIfNullOrWhiteSpace(configuration);
        var moduleName = session.Manifest.LogicModuleName;
        var fileName = OperatingSystem.IsWindows()
            ? moduleName + ".dll"
            : OperatingSystem.IsMacOS() || OperatingSystem.IsMacCatalyst()
                ? "lib" + moduleName + ".dylib"
                : "lib" + moduleName + ".so";
        return Path.Combine(session.LogicOutputDirectory, configuration, fileName);
    }
}
