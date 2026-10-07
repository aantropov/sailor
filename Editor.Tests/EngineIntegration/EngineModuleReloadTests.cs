using SailorEditor.Protocol;
using SailorEditor.Services;
using SailorEditor.Workspace;
using SailorEngine;
using YamlDotNet.RepresentationModel;
using Xunit.Abstractions;

namespace Editor.Tests.EngineIntegration;

public sealed class EngineModuleReloadTests(ITestOutputHelper log)
{
    [NativeEditorFact]
    public async Task Restart_ReplacesModuleTypesAndThePersistedEditorCatalog()
    {
        Assert.True(OperatingSystem.IsMacOS(), "This target exercises the Mac editor service path.");
        var engineRoot = RequiredPath("SAILOR_ENGINE_ROOT");
        var original = RequiredPath("SAILOR_NATIVE_WORKSPACE_FIXTURE");
        var replacement = RequiredPath("SAILOR_NATIVE_WORKSPACE_RELOAD_FIXTURE");
        var workspace = Directory.CreateTempSubdirectory("sailor-editor-module-reload-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(workspace, "Content"));
            var manifestPath = Path.Combine(workspace, "workspace.sailor");
            var serializer = new WorkspaceManifestSerializer();
            var manifest = WorkspaceManifest.CreateDefault("Native editor module reload", engineRoot) with
            {
                LogicModuleName = "WorkspaceFixture"
            };
            await serializer.SaveAsync(manifestPath, manifest);
            var lifecycle = new WorkspaceLifecycleService(serializer, new WorkspaceTemplateService(serializer),
                new RecentWorkspaceStore(Path.Combine(workspace, "Recent.yaml")));
            var opened = await lifecycle.OpenAsync(manifestPath);
            Assert.True(opened.Succeeded, opened.Error);
            var session = opened.Session!;
            var modulePath = Path.Combine(session.LogicOutputDirectory, "Release", "libWorkspaceFixture.dylib");
            Directory.CreateDirectory(Path.GetDirectoryName(modulePath)!);
            File.Copy(original, modulePath);
            var launch = EngineLaunchContract.Resolve(workspace, manifestPath, session.ContentDirectory,
                session.CacheDirectory, engineRoot, manifest.WorkspaceId);

            using var host = new NativeEditorHost(workspace, engineRoot, log);
            await using var client = new EngineProtocolClient(new LocalEngineProtocolTransport(
                host, (endpoint, token) => new ClientWebSocketEngineProtocolTransport(endpoint, token)));
            await using var service = new EngineService(lifecycle, client);
            var publishedWorlds = new List<string>();
            service.OnUpdateCurrentWorldAction += publishedWorlds.Add;
            await service.StartAsync(launch, false, ["--null-audio", "--no-title-stats"]);
            Assert.Equal(EngineLifecycleState.Running, service.State);
            Assert.Single(host.ProcessIds);
            const string initialType = "WorkspaceFixture::FixtureComponent";
            const string reloadedType = "WorkspaceFixture::ReloadedComponent";
            var originalTypes = service.EngineTypes;
            Assert.Contains(initialType, originalTypes.Components.Keys);
            Assert.DoesNotContain(reloadedType, originalTypes.Components.Keys);
            var oldIdentity = EditorTypeCacheStore.ParseNativeIdentity(await client.SerializeWorkspaceCacheIdentityAsync());
            var cache = new EditorTypeCacheStore();
            Assert.Equal(EditorTypeCacheStatus.Loaded, cache.Load(launch.EditorTypesCacheFilePath, oldIdentity).Status);
            var recoveryWorld = await service.SerializeCurrentWorldAsync();
            Assert.False(string.IsNullOrWhiteSpace(recoveryWorld));
            Assert.Empty(publishedWorlds);

            var originalObject = await client.CreateGameObjectAsync("", "");
            Assert.True(originalObject.Succeeded);
            Assert.True((await client.AddComponentAsync(originalObject.InstanceId, initialType, "")).Succeeded);

            await service.StopAsync();
            Assert.Equal(0, host.ExitCode);
            File.Delete(modulePath);
            File.Copy(replacement, modulePath);
            await service.RestartAsync(launch, recoveryWorld);
            Assert.Equal(EngineLifecycleState.Running, service.State);
            Assert.Equal(2, host.ProcessIds.Distinct().Count());
            Assert.Contains(reloadedType, service.EngineTypes.Components.Keys);
            Assert.DoesNotContain(initialType, service.EngineTypes.Components.Keys);
            Assert.Equal(12u, Assert.IsType<Property<uint>>(
                service.EngineTypes.Components[reloadedType].Properties["m_capacity"]).DefaultValue);
            Assert.Contains(initialType, originalTypes.Components.Keys);
            Assert.DoesNotContain(reloadedType, originalTypes.Components.Keys);
            var newIdentity = EditorTypeCacheStore.ParseNativeIdentity(await client.SerializeWorkspaceCacheIdentityAsync());
            Assert.NotEqual(oldIdentity, newIdentity);
            Assert.Equal(oldIdentity.BuildIdentity, newIdentity.BuildIdentity);
            Assert.Equal(oldIdentity.WorkspaceIdentity, newIdentity.WorkspaceIdentity);
            Assert.NotEqual(oldIdentity.ProducerIdentity, newIdentity.ProducerIdentity);
            Assert.Equal(EditorTypeCacheStatus.StaleIdentity, cache.Load(launch.EditorTypesCacheFilePath, oldIdentity).Status);
            var persisted = cache.Load(launch.EditorTypesCacheFilePath, newIdentity);
            Assert.Equal(EditorTypeCacheStatus.Loaded, persisted.Status);
            var cachedTypes = EngineTypes.FromYaml(persisted.Payload!);
            Assert.Contains(reloadedType, cachedTypes.Components.Keys);
            Assert.DoesNotContain(initialType, cachedTypes.Components.Keys);
            MainThread.Drain();
            Assert.Single(publishedWorlds);

            var gameObject = await client.CreateGameObjectAsync("", "");
            Assert.True(gameObject.Succeeded);
            var component = await client.AddComponentAsync(gameObject.InstanceId, reloadedType, "");
            Assert.True(component.Succeeded);
            var populatedWorld = await service.SerializeCurrentWorldAsync();
            AssertReloadedComponent(populatedWorld, reloadedType);

            // Restart once more from Running, retaining the same rebuilt module and cache identity.
            // Export timestamps have second precision and must not invalidate the catalog.
            await Task.Delay(TimeSpan.FromSeconds(1.1));
            await service.RestartAsync(launch, populatedWorld);
            Assert.Equal(3, host.ProcessIds.Distinct().Count());
            Assert.Contains(reloadedType, service.EngineTypes.Components.Keys);
            Assert.DoesNotContain(initialType, service.EngineTypes.Components.Keys);
            Assert.Equal(newIdentity, EditorTypeCacheStore.ParseNativeIdentity(await client.SerializeWorkspaceCacheIdentityAsync()));
            Assert.Equal(EditorTypeCacheStatus.Loaded, cache.Load(launch.EditorTypesCacheFilePath, newIdentity).Status);
            AssertReloadedComponent(await service.SerializeCurrentWorldAsync(), reloadedType);
            await service.StopAsync();
            Assert.Equal(0, host.ExitCode);

            // An unavailable module must not revive the cached game catalog in editor-only mode.
            File.Delete(modulePath);
            await service.RestartAsync(launch, recoveryWorld);
            Assert.Equal(4, host.ProcessIds.Distinct().Count());
            Assert.Equal(EngineLifecycleState.Running, service.State);
            Assert.DoesNotContain(initialType, service.EngineTypes.Components.Keys);
            Assert.DoesNotContain(reloadedType, service.EngineTypes.Components.Keys);
            var engineOnlyIdentity = EditorTypeCacheStore.ParseNativeIdentity(await client.SerializeWorkspaceCacheIdentityAsync());
            Assert.NotEqual(newIdentity, engineOnlyIdentity);
            Assert.Equal(EditorTypeCacheStatus.Loaded, cache.Load(launch.EditorTypesCacheFilePath, engineOnlyIdentity).Status);
            await service.StopAsync();
            Assert.Equal(0, host.ExitCode);

            File.Copy(replacement, modulePath);
            await service.RestartAsync(launch, populatedWorld);
            Assert.Equal(5, host.ProcessIds.Distinct().Count());
            Assert.Contains(reloadedType, service.EngineTypes.Components.Keys);
            Assert.Equal(newIdentity, EditorTypeCacheStore.ParseNativeIdentity(await client.SerializeWorkspaceCacheIdentityAsync()));
            AssertReloadedComponent(await service.SerializeCurrentWorldAsync(), reloadedType);
            await service.StopAsync();
            Assert.Equal(0, host.ExitCode);
            MainThread.Drain();
        }
        finally
        {
            MainThread.Drain();
            Directory.Delete(workspace, recursive: true);
        }
    }

    static void AssertReloadedComponent(string world, string typeName)
    {
        var document = new YamlStream();
        document.Load(new StringReader(world));
        var root = (YamlMappingNode)Assert.Single(document.Documents).RootNode;
        var components = ((YamlSequenceNode)root["prefabs"])
            .Cast<YamlMappingNode>()
            .SelectMany(prefab => (YamlSequenceNode)prefab["components"])
            .Cast<YamlMappingNode>();
        var component = Assert.Single(components, value => value["typename"].ToString() == typeName);
        Assert.Equal("12", ((YamlMappingNode)component["overrideProperties"])["m_capacity"].ToString());
    }

    static string RequiredPath(string name) => Path.GetFullPath(Environment.GetEnvironmentVariable(name)
        ?? throw new InvalidOperationException($"{name} is required for native module reload integration."));
}
