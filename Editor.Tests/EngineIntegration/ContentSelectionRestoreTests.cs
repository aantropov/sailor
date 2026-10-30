using SailorEditor;
using SailorEditor.Content;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workspace;

namespace Editor.Tests.EngineIntegration;

[Collection("LocalEngineProtocolTransport")]
public sealed class ContentSelectionRestoreTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DirtyDescendantKeepsEditsAndAdoptsLocationAfterSelectedFolderMutation(bool move)
    {
        var directory = Directory.CreateTempSubdirectory("sailor-content-selection-restore-");
        try
        {
            var serializer = new WorkspaceManifestSerializer();
            var lifecycle = new WorkspaceLifecycleService(serializer, new WorkspaceTemplateService(serializer),
                new RecentWorkspaceStore(Path.Combine(directory.FullName, "Recent.yaml")));
            await using var engine = new EngineService(lifecycle);
            MauiProgram.SetService(engine);
            var selection = new SelectionService();
            MauiProgram.SetService(selection);
            using var assets = new AssetsService();
            var content = ProjectContentPathPolicy.NormalizeRoot(
                Directory.CreateDirectory(Path.Combine(directory.FullName, "Content")).FullName);
            var sourceFolder = Directory.CreateDirectory(Path.Combine(content, "Original")).FullName;
            var nested = Directory.CreateDirectory(Path.Combine(sourceFolder, "Nested")).FullName;
            var destination = Directory.CreateDirectory(Path.Combine(content, "Destination")).FullName;
            var source = Path.Combine(nested, "Note.txt");
            File.WriteAllText(source, "Content selection restore fixture");
            File.WriteAllText(source + ".asset", """
                assetInfoType: Sailor::AssetInfo
                fileId: '{00000000-0000-0000-0000-000000000303}'
                filename: Note.txt
                custom: before
                """);
            assets.AddProjectRoot(new EngineLaunchContext(directory.FullName,
                Path.Combine(directory.FullName, "workspace.sailor"), content,
                Path.Combine(directory.FullName, "Cache"), "Restore", EditorProjectMode.Workspace));
            var store = new ProjectContentStore(assets);
            MauiProgram.SetService(store);
            var folder = Assert.IsType<AssetFolder>(await assets.ResolveFolderAsync(sourceFolder));
            var destinationFolder = Assert.IsType<AssetFolder>(await assets.ResolveFolderAsync(destination));
            var nestedFolder = Assert.IsType<AssetFolder>(await assets.ResolveFolderAsync(nested));
            await assets.EnsureFolderLoadedAsync(nestedFolder.Id);
            var original = Assert.IsType<AssetFile>(Assert.Single(assets.Files));
            await selection.SelectObjectAsync(original);
            original.ReadOnlyAssetProperties.Remove("custom");
            var edited = Assert.IsType<Observable<string>>(original.AssetProperties["custom"]);
            edited.Value = "edited";
            store.SelectFolder(folder.Id);

            var result = move ? assets.MoveFolder(folder, destinationFolder) : assets.RenameFolder(folder, "Renamed");

            Assert.True(result.Succeeded, result.Error);
            Assert.Null(selection.SelectedItem);
            var expectedFolder = move ? Path.Combine(destination, "Original") : Path.Combine(content, "Renamed");
            var restored = await ProjectContentSelectionRestorePolicy.ResolveAssetAsync(assets, original,
                new Dictionary<string, string>(ProjectContentPathPolicy.PathComparer) { [sourceFolder] = expectedFolder });
            var refreshed = Assert.IsType<AssetFile>(await assets.ResolveAssetAsync(original.FileId));
            Assert.Same(original, restored);
            Assert.True(original.IsDirty);
            Assert.Same(edited, original.AssetProperties["custom"]);
            var expectedMetadata = Path.Combine(expectedFolder, "Nested", "Note.txt.asset");
            Assert.True(ProjectContentPathPolicy.IsSamePath(expectedMetadata, original.AssetInfo.FullName));
            Assert.Equal(refreshed.FolderId, original.FolderId);
            await original.Save();
            Assert.Contains("custom: edited", File.ReadAllText(expectedMetadata));
            Assert.False(File.Exists(source + ".asset"));
        }
        finally
        {
            MauiProgram.ClearServices();
            directory.Delete(true);
        }
    }
}
