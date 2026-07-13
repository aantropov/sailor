using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceUiProjectionTests
{
    [Fact]
    public void Build_UsesFallbackIdentity_WhenNoWorkspaceIsActive()
    {
        var recent = new[]
        {
            new RecentWorkspaceEntry("id-1", "Sandbox", "/tmp/Sandbox/workspace.sailor", new DateTimeOffset(2026, 6, 25, 12, 0, 0, TimeSpan.Zero))
        };

        var projection = WorkspaceUiProjectionBuilder.Build(null, recent);

        Assert.False(projection.HasActiveWorkspace);
        Assert.Equal("No workspace", projection.ActiveWorkspaceName);
        Assert.Equal("Repository fallback", projection.ActiveWorkspacePath);
        Assert.Equal("No workspace", projection.ToolbarText);
        Assert.Single(projection.RecentWorkspaces);
    }

    [Fact]
    public void Build_UsesCurrentWorkspaceIdentity()
    {
        var root = Path.GetFullPath(Path.Combine(Path.GetTempPath(), "sailor-ui-projection"));
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var session = new WorkspaceSession(
            root,
            Path.Combine(root, WorkspaceTemplateService.ManifestFileName),
            manifest,
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            Path.Combine(root, "Cache"));

        var projection = WorkspaceUiProjectionBuilder.Build(session, []);

        Assert.True(projection.HasActiveWorkspace);
        Assert.Equal("Sandbox", projection.ActiveWorkspaceName);
        Assert.Equal(root, projection.ActiveWorkspacePath);
        Assert.Equal("Sandbox", projection.ToolbarText);
    }

    [Fact]
    public void Build_NormalizesRecentWorkspaceItems()
    {
        var openedAt = new DateTimeOffset(2026, 6, 25, 12, 0, 0, TimeSpan.Zero);
        var manifestPath = Path.Combine(Path.GetTempPath(), "Sandbox", "workspace.sailor");

        var projection = WorkspaceUiProjectionBuilder.Build(null, [
            new RecentWorkspaceEntry("id-1", "", manifestPath, openedAt),
            new RecentWorkspaceEntry("id-2", "Second", "", openedAt.AddMinutes(1))
        ]);

        var recent = Assert.Single(projection.RecentWorkspaces);
        Assert.Equal("workspace", recent.Name);
        Assert.Equal(Path.GetFullPath(manifestPath), recent.ManifestPath);
        Assert.Equal(Path.Combine("Sandbox", "workspace.sailor"), recent.DisplayPath);
        Assert.Equal(openedAt, recent.LastOpenedAt);
    }

    [Fact]
    public void Build_ProjectsExplicitRepairState()
    {
        var state = new WorkspaceActivationState(
            WorkspaceActivationPhase.Repair,
            4,
            Error: "Native startup failed.");

        var projection = WorkspaceUiProjectionBuilder.Build(null, [], state);

        Assert.True(projection.RequiresRepair);
        Assert.False(projection.IsActivationInProgress);
        Assert.Equal("Native startup failed.", projection.ActivationError);
    }

    [Fact]
    public void Build_ProjectsGeneratedStateAttentionWithoutRepair()
    {
        const string guidance = "Generated project inputs are stale; user-owned files were not changed.";
        var session = CreateSession(new WorkspaceGeneratedProjectStateAssessment(
            WorkspaceGeneratedProjectStateStatus.Stale,
            guidance,
            [new WorkspaceGeneratedProjectStateMismatch("logicModuleName", "OldGame", "SailorGame")]));

        var projection = WorkspaceUiProjectionBuilder.Build(session, []);

        Assert.True(projection.HasActiveWorkspace);
        Assert.True(projection.HasGeneratedProjectAttention);
        Assert.Equal(guidance, projection.GeneratedProjectAttention);
        Assert.False(projection.RequiresRepair);
    }

    [Fact]
    public void Build_PreservesGeneratedStateAttentionAlongsideRepair()
    {
        const string guidance = "Generated project state is untracked; no files were changed.";
        var session = CreateSession(new WorkspaceGeneratedProjectStateAssessment(
            WorkspaceGeneratedProjectStateStatus.Untracked,
            guidance,
            []));
        var activation = new WorkspaceActivationState(
            WorkspaceActivationPhase.Repair,
            5,
            Error: "Native startup failed.");

        var projection = WorkspaceUiProjectionBuilder.Build(session, [], activation);

        Assert.True(projection.RequiresRepair);
        Assert.True(projection.HasGeneratedProjectAttention);
        Assert.Equal(guidance, projection.GeneratedProjectAttention);
        Assert.Equal("Native startup failed.", projection.ActivationError);
    }

    [Theory]
    [InlineData(WorkspaceActivationPhase.Preflighting)]
    [InlineData(WorkspaceActivationPhase.Stopping)]
    [InlineData(WorkspaceActivationPhase.Clearing)]
    [InlineData(WorkspaceActivationPhase.Committing)]
    [InlineData(WorkspaceActivationPhase.Starting)]
    public void Build_ProjectsInProgressActivation(WorkspaceActivationPhase phase)
    {
        var projection = WorkspaceUiProjectionBuilder.Build(
            null,
            [],
            new WorkspaceActivationState(phase, 1));

        Assert.True(projection.IsActivationInProgress);
        Assert.False(projection.RequiresRepair);
    }

    static WorkspaceSession CreateSession(WorkspaceGeneratedProjectStateAssessment assessment)
    {
        var root = Path.GetFullPath(Path.Combine(Path.GetTempPath(), "sailor-ui-projection-state"));
        return new WorkspaceSession(
            root,
            Path.Combine(root, WorkspaceTemplateService.ManifestFileName),
            WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id"),
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            Path.Combine(root, "Cache"))
        {
            GeneratedProjectState = assessment
        };
    }
}
