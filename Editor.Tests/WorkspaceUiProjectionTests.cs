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
}
