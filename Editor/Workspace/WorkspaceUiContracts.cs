using SailorEditor.Services;

namespace SailorEditor.Workspace;

public sealed record WorkspaceRecentItem(
    string Name,
    string ManifestPath,
    string DisplayPath,
    DateTimeOffset LastOpenedAt);

public sealed record WorkspaceUiProjection(
    string ActiveWorkspaceName,
    string ActiveWorkspacePath,
    bool HasActiveWorkspace,
    IReadOnlyList<WorkspaceRecentItem> RecentWorkspaces,
    WorkspaceActivationPhase ActivationPhase = WorkspaceActivationPhase.Idle,
    string? ActivationError = null,
    string? GeneratedProjectAttention = null)
{
    public static WorkspaceUiProjection Empty { get; } = new("Engine Mode", string.Empty, false, [])
    {
        Mode = EditorProjectMode.Engine
    };

    public EditorProjectMode Mode { get; init; } = EditorProjectMode.Engine;
    public string ActiveRootPath { get; init; } = string.Empty;
    public string ActiveContentPath { get; init; } = string.Empty;
    public string ModeLabel => Mode == EditorProjectMode.Engine ? "Engine Mode" : "Workspace";
    public string ActiveProjectName => Mode == EditorProjectMode.Engine ? "Sailor Engine" : ActiveWorkspaceName;
    public string ToolbarText => Mode == EditorProjectMode.Engine ? "Engine Mode" : ActiveWorkspaceName;
    public string WindowTitle
    {
        get
        {
            var title = Mode == EditorProjectMode.Engine
                ? "Engine Mode"
                : $"Workspace: {ActiveWorkspaceName}";
            if (!string.IsNullOrWhiteSpace(ActiveRootPath))
                title = $"{title} — {ActiveRootPath}";
            return RequiresRepair ? $"{title} — Repair required" : title;
        }
    }
    public bool IsActivationInProgress => ActivationPhase is
        WorkspaceActivationPhase.Preflighting or
        WorkspaceActivationPhase.Stopping or
        WorkspaceActivationPhase.Clearing or
        WorkspaceActivationPhase.Committing or
        WorkspaceActivationPhase.Starting;
    public bool RequiresRepair => ActivationPhase == WorkspaceActivationPhase.Repair;
    public bool HasGeneratedProjectAttention => !string.IsNullOrWhiteSpace(GeneratedProjectAttention);
}

public static class WorkspaceUiProjectionBuilder
{
    public static WorkspaceUiProjection Build(
        WorkspaceSession? current,
        IReadOnlyList<RecentWorkspaceEntry> recentWorkspaces,
        WorkspaceActivationState? activation = null,
        EngineLaunchContext? projectContext = null)
    {
        var recent = recentWorkspaces
            .Where(x => !string.IsNullOrWhiteSpace(x.ManifestPath))
            .Select(x => new WorkspaceRecentItem(
                string.IsNullOrWhiteSpace(x.Name) ? Path.GetFileNameWithoutExtension(x.ManifestPath) : x.Name,
                Path.GetFullPath(x.ManifestPath),
                CompactPath(x.ManifestPath),
                x.LastOpenedAt))
            .ToArray();

        var activationPhase = activation?.Phase ?? WorkspaceActivationPhase.Idle;
        var activationError = activation?.Error;
        if (current is null)
        {
            var activeRootPath = projectContext?.ActiveRootPath ?? string.Empty;
            return new WorkspaceUiProjection(
                "Engine Mode",
                activeRootPath,
                false,
                recent,
                activationPhase,
                activationError)
            {
                Mode = EditorProjectMode.Engine,
                ActiveRootPath = activeRootPath,
                ActiveContentPath = projectContext?.ContentDirectory ?? string.Empty
            };
        }

        var projection = new WorkspaceUiProjection(
            string.IsNullOrWhiteSpace(current.Manifest.Name) ? Path.GetFileName(current.WorkspaceRoot) : current.Manifest.Name,
            current.WorkspaceRoot,
            true,
            recent,
            activationPhase,
            activationError,
            current.GeneratedProjectState.RequiresAttention
                ? current.GeneratedProjectState.Guidance
                : null);
        return projection with
        {
            Mode = EditorProjectMode.Workspace,
            ActiveRootPath = projectContext?.ActiveRootPath ?? current.WorkspaceRoot,
            ActiveContentPath = projectContext?.ContentDirectory ?? current.ContentDirectory
        };
    }

    static string CompactPath(string path)
    {
        try
        {
            var fullPath = Path.GetFullPath(path);
            var directory = Path.GetDirectoryName(fullPath);
            return string.IsNullOrWhiteSpace(directory)
                ? fullPath
                : Path.Combine(Path.GetFileName(directory), Path.GetFileName(fullPath));
        }
        catch
        {
            return path;
        }
    }
}
