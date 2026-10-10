using System.Globalization;
using System.Text.Json.Nodes;
using SailorEditor.Protocol;
using SailorEditor.Workspace;
using YamlDotNet.RepresentationModel;
using Xunit.Abstractions;
#if MACCATALYST
using SailorEditor;
using SailorEditor.Content;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using Xunit.Sdk;
#endif

namespace Editor.Tests;

[Collection("LocalEngineProtocolTransport")]
public sealed class AssetReimportIntegrationTests(ITestOutputHelper log)
{
    const string ModelId = "{00000000-0000-0000-0000-000000000204}";

    [NativeEditorFact]
    public async Task Reimport_RepairsGeneratedMaterialsThroughTheNativeHost()
    {
        var engine = Path.GetFullPath(Environment.GetEnvironmentVariable("SAILOR_ENGINE_ROOT")
            ?? throw new InvalidOperationException("SAILOR_ENGINE_ROOT is required for native integration tests."));
        var workspace = Directory.CreateTempSubdirectory("sailor-editor-reimport-").FullName;
        try
        {
            var content = Directory.CreateDirectory(Path.Combine(workspace, "Content")).FullName;
            var manifestPath = Path.Combine(workspace, "workspace.sailor");
            var serializer = new WorkspaceManifestSerializer();
            var manifest = WorkspaceManifest.CreateDefault("Editor reimport integration", engine);
            await serializer.SaveAsync(manifestPath, manifest);
            var sourcePath = Path.Combine(content, "Panel.gltf");
            var source = JsonNode.Parse("""
                {
                  "asset": {"version": "2.0"},
                  "buffers": [{"uri": "Panel.bin", "byteLength": 78}],
                  "bufferViews": [{"buffer": 0, "byteLength": 36},
                    {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                    {"buffer": 0, "byteOffset": 72, "byteLength": 6}],
                  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
                      "min": [-1,-1,0], "max": [1,1,0]},
                    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
                    {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}],
                  "materials": [{"name": "Glass", "emissiveFactor": [0.1, 0.2, 0.3],
                    "pbrMetallicRoughness": {"roughnessFactor": 0.6}}],
                  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
                  "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0
                }
                """)!;
            using (var geometry = new BinaryWriter(File.Create(Path.Combine(content, "Panel.bin"))))
            {
                foreach (var value in new float[] { -1, -1, 0, 1, -1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1 })
                {
                    geometry.Write(value);
                }
                foreach (var index in new ushort[] { 0, 1, 2 })
                {
                    geometry.Write(index);
                }
            }
            await File.WriteAllTextAsync(sourcePath, source.ToJsonString());
            await File.WriteAllTextAsync(sourcePath + ".asset", $$"""
                assetInfoType: Sailor::ModelAssetInfo
                fileId: '{{ModelId}}'
                filename: Panel.gltf
                bShouldGenerateMaterials: true
                bGenerateLods: false
                unitScale: 1
                """);

            using var host = new NativeEditorHost(workspace, engine, log);
            await using var client = new EngineProtocolClient(new LocalEngineProtocolTransport(
                host, (endpoint, token) => new ClientWebSocketEngineProtocolTransport(endpoint, token)));
#if MACCATALYST
            // The service target runs the same material checks through the production editor service.
            Assert.True(OperatingSystem.IsMacOS());
            var lifecycle = new WorkspaceLifecycleService(serializer, new WorkspaceTemplateService(serializer),
                new RecentWorkspaceStore(Path.Combine(workspace, "Recent.yaml")));
            var opened = await lifecycle.OpenAsync(manifestPath);
            Assert.True(opened.Succeeded, opened.Error);
            var session = opened.Session!;
            var launch = EngineLaunchContract.Resolve(workspace, manifestPath, session.ContentDirectory,
                session.CacheDirectory, engine, manifest.WorkspaceId);
            await using var service = new EngineService(lifecycle, client);
            await service.StartAsync(launch, false, ["--null-audio", "--no-title-stats"]);
            Assert.Equal(EngineLifecycleState.Running, service.State);
            Func<Task<bool>> reimport = () => service.ReimportAssetAsync(ModelId);
            Func<Task<bool>> update = () => service.UpdateAssetAsync(ModelId);
#else
            await client.InitializeAsync([
                "SailorEditorIntegration", "--workspace", workspace, "--editor", "--new-world",
                "--port", "0", "--noconsole", "--null-audio", "--no-title-stats"
            ]);
            await client.StartAsync();
            using (var started = new CancellationTokenSource(TimeSpan.FromSeconds(5)))
            {
                while (!await client.IsEngineRunningAsync(started.Token))
                {
                    await Task.Delay(10, started.Token);
                }
            }
            Func<Task<bool>> reimport = () => client.ReimportAssetAsync(ModelId);
            Func<Task<bool>> update = () => client.UpdateAssetAsync(ModelId);
#endif

            Assert.True(await reimport());
            var beforeLoad = ReadContent(content);
            var instance = await client.CreateModelInstanceAsync(ModelId, "Panel", "", false, null, "");
            Assert.True(instance.Succeeded);
            Assert.False(string.IsNullOrEmpty(instance.InstanceId));
            Assert.True(await update());
            AssertContentUnchanged(beforeLoad, content);
            var materialPath = Assert.Single(Directory.GetFiles(content, "*.mat", SearchOption.AllDirectories));
            var materialInfoPath = materialPath + ".asset";
            var materialId = Scalar(ReadYaml(materialInfoPath), "fileId");
            var material = ReadYaml(materialPath);
            var authoredShader = Scalar(material, "shaderUid");
            ((YamlMappingNode)material["uniformsFloat"]).Children["material.roughnessFactor"] = new YamlScalarNode("0.91");
            WriteYaml(materialPath, material);

            source["extensionsUsed"] = new JsonArray("KHR_materials_transmission", "KHR_materials_ior", "KHR_materials_emissive_strength");
            var generated = source["materials"]![0]!;
            generated["emissiveFactor"] = new JsonArray(0.5, 0.25, 0.125);
            generated["extensions"] = JsonNode.Parse("""
                {"KHR_materials_transmission": {"transmissionFactor": 0.8},
                 "KHR_materials_ior": {"ior": 1.4},
                 "KHR_materials_emissive_strength": {"emissiveStrength": 4}}
                """);
            await File.WriteAllTextAsync(sourcePath, source.ToJsonString());
            Assert.True(await reimport());
            material = ReadYaml(materialPath);
            Assert.Equal(materialId, Scalar(ReadYaml(materialInfoPath), "fileId"));
            Assert.Equal(authoredShader, Scalar(material, "shaderUid"));
            Assert.Equal("Transparent", Scalar(material, "renderQueue"));
            Assert.Equal(0.91f, Number(material, "uniformsFloat", "material.roughnessFactor"));
            Assert.Equal(0.8f, Number(material, "uniformsFloat", "material.transmissionFactor"));
            Assert.Equal(1.4f, Number(material, "uniformsFloat", "material.indexOfRefraction"));
            Assert.Equal(2f, float.Parse(((YamlSequenceNode)((YamlMappingNode)material["uniformsVec4"])
                ["material.emissiveFactor"])[0].ToString(), CultureInfo.InvariantCulture));

            var sourceBytes = await File.ReadAllBytesAsync(sourcePath);
            var sourceTime = File.GetLastWriteTimeUtc(sourcePath);
            ((YamlMappingNode)material["uniformsFloat"]).Children["material.transmissionFactor"] = new YamlScalarNode("0.1");
            WriteYaml(materialPath, material);
            Assert.True(await update());
            Assert.Equal(0.1f, Number(ReadYaml(materialPath), "uniformsFloat", "material.transmissionFactor"));
            Assert.True(await reimport());
            Assert.Equal(0.8f, Number(ReadYaml(materialPath), "uniformsFloat", "material.transmissionFactor"));
            Assert.Equal(sourceBytes, await File.ReadAllBytesAsync(sourcePath));
            Assert.Equal(sourceTime, File.GetLastWriteTimeUtc(sourcePath));

            var valid = await File.ReadAllTextAsync(materialPath);
            await File.WriteAllTextAsync(materialPath, "uniformsFloat: [");
            Assert.False(await reimport());
            Assert.False(await reimport());
            await File.WriteAllTextAsync(materialPath, valid);
            Assert.True(await reimport());
            Assert.Equal(materialId, Scalar(ReadYaml(materialInfoPath), "fileId"));

            File.Delete(materialPath);
            Assert.True(await reimport());
            Assert.Equal(materialId, Scalar(ReadYaml(materialInfoPath), "fileId"));
            Assert.Equal(0.8f, Number(ReadYaml(materialPath), "uniformsFloat", "material.transmissionFactor"));
            var repaired = await File.ReadAllBytesAsync(materialPath);
            var repairedTime = File.GetLastWriteTimeUtc(materialPath);
            Assert.True(await reimport());
            Assert.Equal(repaired, await File.ReadAllBytesAsync(materialPath));
            Assert.Equal(repairedTime, File.GetLastWriteTimeUtc(materialPath));

#if MACCATALYST
            MainThread.Drain();
            MauiProgram.SetService(service);
            using var assets = new AssetsService();
            MauiProgram.SetService(assets);
            var selection = new SelectionService();
            MauiProgram.SetService(selection);
            await assets.EnsureFolderLoadedAsync(ProjectContentFolderIds.ContentRootId);
            var selected = Assert.IsType<ModelFile>(await assets.ResolveAssetAsync(new FileId(ModelId)));
            Assert.True(assets.CanReimportAsset(selected));
            Assert.True(await assets.ReimportAssetAsync(selected));
            await selection.SelectObjectAsync(selected);
            Assert.Same(selected, selection.SelectedItem);

            var pendingAsset = new PendingInspectorAsset
            {
                FileId = new FileId(materialId),
                Asset = new FileInfo(materialPath),
                AssetInfo = new FileInfo(materialInfoPath)
            };
            await pendingAsset.EnsureMetadataLoadedAsync();
            Assert.Empty(pendingAsset.LoadError);
            var completions = new List<AssetReloadCompletion>();
            service.OnAssetReloadCompleted += completions.Add;
            foreach (var changeSelection in new[] { false, true })
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
                completions.Clear();
                MainThread.QueueInvocations = true;
                var context = new AsyncTestSyncContext(SynchronizationContext.Current);
                var reload = service.RequestAssetReloadAsync(timeout.Token);
                while (completions.Count == 0)
                {
                    // Stop dispatching after the real completion starts its queued tree refresh.
                    var previousContext = SynchronizationContext.Current;
                    try
                    {
                        SynchronizationContext.SetSynchronizationContext(context);
                        MainThread.DispatchNext();
                    }
                    finally
                    {
                        SynchronizationContext.SetSynchronizationContext(previousContext);
                    }
                    await Task.Delay(10, timeout.Token);
                }
                var refresh = context.WaitForCompletionAsync();
                Assert.False(refresh.IsCompleted);
                var pendingSelection = changeSelection ? selection.SelectObjectAsync(pendingAsset) : Task.CompletedTask;
                if (changeSelection)
                {
                    Assert.Equal(materialId, selection.Snapshot.SelectedId);
                    Assert.Same(selected, selection.SelectedItem);
                    Assert.False(pendingSelection.IsCompleted);
                }
                MainThread.QueueInvocations = false;
                while (!refresh.IsCompleted)
                {
                    MainThread.Drain();
                    await Task.Delay(10, timeout.Token);
                }
                Assert.Null(await refresh);
                Assert.True(await reload.WaitAsync(timeout.Token));
                var state = await client.GetAssetReloadStateAsync();
                Assert.True(state.Available);
                Assert.Equal(new AssetReloadCompletion(state.CompletedGeneration, true), Assert.Single(completions));
                var refreshed = Assert.IsType<ModelFile>(await assets.ResolveAssetAsync(new FileId(ModelId)));
                Assert.NotSame(selected, refreshed);
                if (changeSelection)
                {
                    Assert.Equal(materialId, selection.Snapshot.SelectedId);
                    Assert.False(pendingSelection.IsCompleted);
                    pendingAsset.Ready.SetResult();
                    await pendingSelection.WaitAsync(timeout.Token);
                    Assert.Same(pendingAsset, selection.SelectedItem);
                }
                else
                {
                    Assert.Same(refreshed, selection.SelectedItem);
                }
                selected = refreshed;
            }
            service.OnAssetReloadCompleted -= completions.Add;
            await service.StopAsync();
            Assert.Equal(EngineLifecycleState.Stopped, service.State);
            var stoppedContent = ReadContent(content);
            Assert.False(assets.CanReimportAsset(selected));
            Assert.False(await assets.ReimportAssetAsync(selected));
            AssertContentUnchanged(stoppedContent, content);
#endif
            await client.DisposeAsync();
            Assert.Equal(0, host.ExitCode);
            Assert.Contains("Managed editor protocol host completed", host.Output);
        }
        finally
        {
#if MACCATALYST
            MainThread.QueueInvocations = false;
            MainThread.Drain();
            MauiProgram.ClearServices();
#endif
            Directory.Delete(workspace, recursive: true);
        }
    }

#if MACCATALYST
    sealed class PendingInspectorAsset : AssetFile
    {
        public TaskCompletionSource Ready { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public override Task PrepareInspectorResources() => Ready.Task;
    }
#endif

    static YamlMappingNode ReadYaml(string path)
    {
        var document = new YamlStream();
        using var reader = File.OpenText(path);
        document.Load(reader);
        return (YamlMappingNode)document.Documents[0].RootNode;
    }

    static Dictionary<string, (byte[] Bytes, DateTime Modified)> ReadContent(string content) =>
        Directory.GetFiles(content, "*", SearchOption.AllDirectories).ToDictionary(
            path => path, path => (File.ReadAllBytes(path), File.GetLastWriteTimeUtc(path)));

    static void AssertContentUnchanged(Dictionary<string, (byte[] Bytes, DateTime Modified)> expected, string content)
    {
        var actual = ReadContent(content);
        Assert.Equal(expected.Keys.Order(), actual.Keys.Order());
        foreach (var entry in expected)
        {
            Assert.Equal(entry.Value.Bytes, actual[entry.Key].Bytes);
            Assert.Equal(entry.Value.Modified, actual[entry.Key].Modified);
        }
    }

    static void WriteYaml(string path, YamlMappingNode value)
    {
        using var writer = File.CreateText(path);
        new YamlStream(new YamlDocument(value)).Save(writer, assignAnchors: false);
    }

    static string Scalar(YamlMappingNode value, string name) => value[name].ToString();

    static float Number(YamlMappingNode value, string group, string name) =>
        float.Parse(((YamlMappingNode)value[group])[name].ToString(), CultureInfo.InvariantCulture);

}
