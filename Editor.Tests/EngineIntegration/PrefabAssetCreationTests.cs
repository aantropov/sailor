using SailorEditor;
using SailorEditor.Content;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workspace;
using SailorEngine;
using YamlDotNet.RepresentationModel;

namespace Editor.Tests.EngineIntegration;

[Collection("LocalEngineProtocolTransport")]
public sealed class PrefabAssetCreationTests : IAsyncLifetime
{
    const string SourceYaml = """
        gameObjects:
          - name: Original
            instanceId: source-object
            parentIndex: 4294967295
            components: []
        components: []
        """;

    readonly DirectoryInfo directory = Directory.CreateTempSubdirectory("sailor-prefab-publication-");
    EngineService engine = null!;
    AssetsService assets = null!;
    string contentRoot = null!;

    public Task InitializeAsync()
    {
        var serializer = new WorkspaceManifestSerializer();
        var lifecycle = new WorkspaceLifecycleService(serializer, new WorkspaceTemplateService(serializer),
            new RecentWorkspaceStore(Path.Combine(directory.FullName, "Recent.yaml")));
        engine = new EngineService(lifecycle);
        MauiProgram.SetService(engine);
        MauiProgram.SetService(new WorldService { PrefabSnapshot = SourceYaml });
        assets = new AssetsService();
        contentRoot = Directory.CreateDirectory(Path.Combine(directory.FullName, "Content")).FullName;
        assets.AddProjectRoot(new EngineLaunchContext(directory.FullName,
            Path.Combine(directory.FullName, "workspace.sailor"), contentRoot,
            Path.Combine(directory.FullName, "Cache"), "Prefab test", EditorProjectMode.Workspace));
        return Task.CompletedTask;
    }

    public async Task DisposeAsync()
    {
        assets.Dispose();
        await engine.DisposeAsync();
        MauiProgram.ClearServices();
        directory.Delete(true);
    }

    [Theory]
    [InlineData(false, false, false)]
    [InlineData(false, false, true)]
    [InlineData(false, true, false)]
    [InlineData(false, true, true)]
    [InlineData(true, false, false)]
    [InlineData(true, false, true)]
    [InlineData(true, true, false)]
    [InlineData(true, true, true)]
    public async Task CreatePrefab_PublishesWithoutRequiringTheDestinationToBeLoaded(
        bool nested, bool loaded, bool commit)
    {
        var folder = await PrepareFolderAsync(nested, loaded);
        var sourcePath = Path.Combine(folder.FullPath, "Created.prefab");
        var creation = Assert.IsType<PrefabAssetWriteResult>(assets.BeginCreatePrefabAsset(
            nested ? folder : null, new GameObject { Name = "Created" }));

        Assert.True(creation.Transaction.Result.Succeeded);
        Assert.Same(creation.Prefab, assets.Assets[creation.Prefab.FileId]);
        Assert.Equal(folder.Id, creation.Prefab.FolderId);
        Assert.False(creation.Prefab.IsReadOnly);
        Assert.True(File.Exists(sourcePath));
        Assert.Equal(SourceYaml, File.ReadAllText(sourcePath));
        var metadata = ReadRoot(File.ReadAllText(sourcePath + ".asset"));
        Assert.Equal(creation.Prefab.FileId.Value, ((YamlScalarNode)metadata["fileId"]).Value);
        Assert.Equal("Sailor::PrefabAssetInfo", ((YamlScalarNode)metadata["assetInfoType"]).Value);

        var result = assets.CompletePrefabAssetWrite(creation.Transaction, commit);

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(commit, File.Exists(sourcePath));
        Assert.Equal(commit, File.Exists(sourcePath + ".asset"));
        Assert.Equal(commit, assets.Assets.ContainsKey(creation.Prefab.FileId));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ApplyOverrides_PublishesAnIndexedPrefabFromAnUnloadedFolder(bool commit)
    {
        var folder = await PrepareFolderAsync(nested: false, loaded: false);
        var sourcePath = Path.Combine(folder.FullPath, "Indexed.prefab");
        var fileId = new FileId(Guid.NewGuid().ToString());
        File.WriteAllText(sourcePath, SourceYaml);
        var metadata = $"assetInfoType: Sailor::PrefabAssetInfo\nfileId: {fileId.Value}\nfilename: Indexed.prefab\n";
        File.WriteAllText(sourcePath + ".asset", metadata);
        var indexed = new PrefabFile
        {
            FileId = fileId, Asset = new FileInfo(sourcePath), AssetInfo = new FileInfo(sourcePath + ".asset"),
            FolderId = folder.Id, ProjectRootId = folder.ProjectRootId
        };
        assets.Files.Add(indexed);
        assets.Assets.Add(fileId, indexed);

        var creation = Assert.IsType<PrefabAssetWriteResult>(assets.BeginApplyPrefabOverrides(indexed, """
            gameObjectOverrides:
              source-object:
                name: Applied
            """));

        Assert.Same(creation.Prefab, assets.Assets[fileId]);
        Assert.NotSame(indexed, creation.Prefab);
        var document = ReadRoot(File.ReadAllText(sourcePath));
        var gameObject = (YamlMappingNode)((YamlSequenceNode)document["gameObjects"]).Children.Single();
        Assert.Equal("Applied", ((YamlScalarNode)gameObject["name"]).Value);
        Assert.Equal(metadata, File.ReadAllText(sourcePath + ".asset"));

        var result = assets.CompletePrefabAssetWrite(creation.Transaction, commit);

        Assert.True(result.Succeeded, result.Error);
        if (!commit)
        {
            Assert.Equal(SourceYaml, File.ReadAllText(sourcePath));
        }
        Assert.Equal(metadata, File.ReadAllText(sourcePath + ".asset"));
    }

    async Task<AssetFolder> PrepareFolderAsync(bool nested, bool loaded)
    {
        var folderId = ProjectContentFolderIds.ContentRootId;
        if (nested)
        {
            var path = Directory.CreateDirectory(Path.Combine(contentRoot, "Prefabs")).FullName;
            await assets.EnsureFolderLoadedAsync(folderId);
            folderId = assets.Folders.Single(folder =>
                ProjectContentPathPolicy.IsSamePath(folder.FullPath, path)).Id;
        }
        if (loaded)
        {
            await assets.EnsureFolderLoadedAsync(folderId);
        }
        var destination = assets.Folders.Single(folder => folder.Id == folderId);
        Assert.Equal(loaded, destination.IsLoaded);
        return destination;
    }

    static YamlMappingNode ReadRoot(string text)
    {
        var yaml = new YamlStream();
        yaml.Load(new StringReader(text));
        return (YamlMappingNode)yaml.Documents.Single().RootNode;
    }
}
