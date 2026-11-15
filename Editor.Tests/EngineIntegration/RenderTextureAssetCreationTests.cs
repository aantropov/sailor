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
public sealed class RenderTextureAssetCreationTests : IAsyncLifetime
{
    readonly DirectoryInfo directory = Directory.CreateTempSubdirectory("sailor-rt-creation-");
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
        MauiProgram.SetService(new WorldService());
        assets = new AssetsService();
        contentRoot = Directory.CreateDirectory(Path.Combine(directory.FullName, "Content")).FullName;
        assets.AddProjectRoot(new EngineLaunchContext(directory.FullName,
            Path.Combine(directory.FullName, "workspace.sailor"), contentRoot,
            Path.Combine(directory.FullName, "Cache"), "RT test", EditorProjectMode.Workspace));
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
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task CreateRenderTexture_PublishesTypedAssetAndSourcePair(bool nested, bool loaded)
    {
        var folder = await PrepareFolderAsync(nested, loaded);
        var created = await assets.CreateRenderTextureAssetAsync(folder);

        Assert.NotNull(created);
        Assert.IsType<RenderTextureFile>(created);
        Assert.True(assets.Files.Contains(created));
        Assert.True(assets.Assets.ContainsKey(created.FileId));
        Assert.Same(created, assets.Assets[created.FileId]);
        Assert.Equal(folder.Id, created.FolderId);
        Assert.True(folder.HasChildren);

        var sourcePath = created.Asset!.FullName;
        var metadataPath = created.AssetInfo!.FullName;
        Assert.True(File.Exists(sourcePath));
        Assert.True(File.Exists(metadataPath));

        var descriptor = RenderTextureDescriptor.Parse(File.ReadAllText(sourcePath));
        Assert.Equal(512, descriptor.Width);
        Assert.Equal(512, descriptor.Height);
        Assert.Equal("R8G8B8A8_UNORM", descriptor.Format);

        var metadata = ReadRoot(File.ReadAllText(metadataPath));
        Assert.Equal("Sailor::TextureAssetInfo", ((YamlScalarNode)metadata["assetInfoType"]).Value);
        Assert.Equal(created.FileId.Value, ((YamlScalarNode)metadata["fileId"]).Value);
        Assert.Equal(Path.GetFileName(sourcePath), ((YamlScalarNode)metadata["filename"]).Value);
        Assert.Equal("Linear", ((YamlScalarNode)metadata["filtration"]).Value);
        Assert.Equal("Clamp", ((YamlScalarNode)metadata["clamping"]).Value);
        Assert.Equal("Average", ((YamlScalarNode)metadata["reduction"]).Value);
        Assert.False(bool.Parse(((YamlScalarNode)metadata["bShouldGenerateMips"]).Value));
        Assert.False(bool.Parse(((YamlScalarNode)metadata["bShouldSupportStorageBinding"]).Value));
        Assert.False(bool.Parse(((YamlScalarNode)metadata["bShouldKeepCpuBuffers"]).Value));
        Assert.False(metadata.Children.Any(child =>
            ((YamlScalarNode)child.Key).Value is "width" or "height" or "version"));
    }

    [Fact]
    public async Task CreateRenderTexture_UniqueNames_ProduceDistinctPathsAndPreserveFirst()
    {
        var folder = await PrepareFolderAsync(nested: false, loaded: true);
        var first = await assets.CreateRenderTextureAssetAsync(folder);
        var firstSource = File.ReadAllText(first!.Asset!.FullName);
        var second = await assets.CreateRenderTextureAssetAsync(folder);

        Assert.NotNull(second);
        Assert.NotEqual(first.FileId, second.FileId);
        Assert.NotEqual(first.Asset!.FullName, second.Asset!.FullName);
        Assert.Equal(firstSource, File.ReadAllText(first.Asset.FullName));
    }

    [Fact]
    public async Task CreateRenderTexture_ReadonlyDestination_ReturnsNullAndWritesNothing()
    {
        var folder = await PrepareFolderAsync(nested: false, loaded: true);
        var before = Directory.GetFiles(folder.FullPath).Length;
        folder.IsReadOnly = true;
        try
        {
            var result = await assets.CreateRenderTextureAssetAsync(folder);

            Assert.Null(result);
            Assert.Equal(before, Directory.GetFiles(folder.FullPath).Length);
        }
        finally
        {
            folder.IsReadOnly = false;
        }
    }

    [Fact]
    public async Task CreateRenderTexture_PrecanceledToken_ThrowsAndCreatesNoFiles()
    {
        var folder = await PrepareFolderAsync(nested: false, loaded: true);
        var before = Directory.GetFiles(folder.FullPath).Length;
        using var cts = new CancellationTokenSource();
        cts.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => assets.CreateRenderTextureAssetAsync(folder, cts.Token));

        Assert.Equal(before, Directory.GetFiles(folder.FullPath).Length);
    }

    async Task<AssetFolder> PrepareFolderAsync(bool nested, bool loaded)
    {
        var folderId = ProjectContentFolderIds.ContentRootId;
        if (nested)
        {
            var path = Directory.CreateDirectory(Path.Combine(contentRoot, "Textures")).FullName;
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
