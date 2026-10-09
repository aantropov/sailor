using SailorEditor;
using SailorEditor.Content;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor.Workspace;

namespace Editor.Tests.EngineIntegration;

[Collection("LocalEngineProtocolTransport")]
public sealed class AssetRefreshTests
{
    public enum WorkspaceChange { None, Switch, Close }

    [Theory]
    [InlineData(WorkspaceChange.None)]
    [InlineData(WorkspaceChange.Switch)]
    [InlineData(WorkspaceChange.Close)]
    public async Task QueuedRefresh_KeepsTheCurrentWorkspace(WorkspaceChange change)
    {
        var directory = Directory.CreateTempSubdirectory("sailor-asset-refresh-");
        try
        {
            var serializer = new WorkspaceManifestSerializer();
            var lifecycle = new WorkspaceLifecycleService(serializer, new WorkspaceTemplateService(serializer),
                new RecentWorkspaceStore(Path.Combine(directory.FullName, "Recent.yaml")));
            await using var engine = new EngineService(lifecycle);
            MauiProgram.SetService(engine);
            using var assets = new AssetsService();
            var first = MakeContext(directory.FullName, "First");
            var second = MakeContext(directory.FullName, "Second");
            var source = Path.Combine(first.ContentDirectory, "Note.txt");
            File.WriteAllText(source, "Asset refresh fixture");
            File.WriteAllText(source + ".asset", """
                assetInfoType: Sailor::AssetInfo
                fileId: '{00000000-0000-0000-0000-000000000301}'
                filename: Note.txt
                """);
            assets.AddProjectRoot(first);
            await assets.EnsureFolderLoadedAsync(ProjectContentFolderIds.ContentRootId);
            var original = Assert.IsType<AssetFile>(Assert.Single(assets.Files));
            var tree = new ProjectContentStore(assets);

            MainThread.QueueInvocations = true;
            var refresh = assets.RefreshAsync();
            Assert.False(refresh.IsCompleted);
            if (change == WorkspaceChange.Close)
            {
                assets.ResetForWorkspaceChange();
            }
            else if (change == WorkspaceChange.Switch)
            {
                assets.AddProjectRoot(second);
            }
            var epoch = assets.WorkspaceEpoch;
            MainThread.QueueInvocations = false;
            MainThread.Drain();
            await refresh.WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(epoch, assets.WorkspaceEpoch);
            var expectedRoot = change switch
            {
                WorkspaceChange.None => first.ContentDirectory,
                WorkspaceChange.Switch => second.ContentDirectory,
                _ => string.Empty
            };
            Assert.Equal(expectedRoot.Length == 0 ? string.Empty : ProjectContentPathPolicy.NormalizeRoot(expectedRoot),
                assets.CurrentProjectRootPath);
            if (change == WorkspaceChange.None)
            {
                var refreshed = Assert.IsType<AssetFile>(Assert.Single(assets.Files));
                Assert.NotSame(original, refreshed);
                Assert.Equal(original.FileId, refreshed.FileId);
                Assert.Contains(tree.Projection.Folders, folder =>
                    ProjectContentPathPolicy.IsSamePath(first.ContentDirectory, folder.FullPath));
            }
            else
            {
                Assert.DoesNotContain(assets.Folders, folder =>
                    ProjectContentPathPolicy.IsInsideRoot(first.ContentDirectory, folder.FullPath));
                Assert.DoesNotContain(tree.Projection.Folders, folder =>
                    ProjectContentPathPolicy.IsInsideRoot(first.ContentDirectory, folder.FullPath));
            }
        }
        finally
        {
            MainThread.QueueInvocations = false;
            MainThread.Drain();
            MauiProgram.ClearServices();
            directory.Delete(true);
        }
    }

    static EngineLaunchContext MakeContext(string directory, string name)
    {
        var root = Directory.CreateDirectory(Path.Combine(directory, name)).FullName;
        var content = Directory.CreateDirectory(Path.Combine(root, "Content")).FullName;
        return new(root, Path.Combine(root, "workspace.sailor"), content,
            Path.Combine(root, "Cache"), name, EditorProjectMode.Workspace);
    }
}
