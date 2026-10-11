using SailorEditor.Protocol;
using SailorEditor.Protocol.Generated;
using SailorEditor.Workspace;
using Xunit.Abstractions;

namespace Editor.Tests;

[Collection("LocalEngineProtocolTransport")]
public sealed class UnsupportedExportIntegrationTests(ITestOutputHelper log)
{
    [NativeEditorFact]
    public async Task PathTracedExport_IsRejectedWithoutChangingTheWorldOrOutput()
    {
        var engine = Path.GetFullPath(Environment.GetEnvironmentVariable("SAILOR_ENGINE_ROOT")
            ?? throw new InvalidOperationException("SAILOR_ENGINE_ROOT is required for native integration tests."));
        var workspace = Directory.CreateTempSubdirectory("sailor-editor-unsupported-export-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(workspace, "Content"));
            await new WorkspaceManifestSerializer().SaveAsync(Path.Combine(workspace, "workspace.sailor"),
                WorkspaceManifest.CreateDefault("Unsupported editor export", engine));

            using var host = new NativeEditorHost(workspace, engine, log);
            await using var client = new EngineProtocolClient(new LocalEngineProtocolTransport(
                host, (endpoint, token) => new ClientWebSocketEngineProtocolTransport(endpoint, token)));
            await client.InitializeAsync([
                "SailorEditorIntegration", "--workspace", workspace, "--editor", "--new-world",
                "--port", "0", "--noconsole", "--null-audio", "--no-title-stats"
            ]);
            await client.StartAsync();
            using (var started = new CancellationTokenSource(TimeSpan.FromSeconds(5)))
            {
                while (!await client.IsEngineRunningAsync(started.Token))
                    await Task.Delay(10, started.Token);
            }

            var gameObject = await client.CreateGameObjectAsync("", "");
            Assert.True(gameObject.Succeeded);
            var component = await client.AddComponentAsync(gameObject.InstanceId, "Sailor::LightComponent", "");
            Assert.True(component.Succeeded);
            var before = await client.SerializeCurrentWorldAsync();
            var outputDirectory = Path.Combine(workspace, "Export");
            var existingOutput = Path.Combine(workspace, "existing-output.data");
            await File.WriteAllTextAsync(existingOutput, "Keep the existing output.");

            foreach (var instanceId in new[] { "", gameObject.InstanceId, component.InstanceId })
            {
                foreach (var outputPath in new[] { Path.Combine(outputDirectory, "image.png"), existingOutput })
                {
                    var error = await Assert.ThrowsAsync<EngineProtocolException>(() => client.SendAsync(new ProtocolRequest
                    {
                        RenderPathTracedImage = new RenderPathTracedImageRequest
                        {
                            OutputPath = outputPath,
                            InstanceId = instanceId,
                            Height = 720,
                            SamplesPerPixel = 64,
                            MaxBounces = 4
                        }
                    }));
                    Assert.Equal("Path-traced image export is not supported by the editor.", error.Message);
                    Assert.False(Directory.Exists(outputDirectory));
                    Assert.Equal("Keep the existing output.", await File.ReadAllTextAsync(existingOutput));
                    Assert.Equal(before, await client.SerializeCurrentWorldAsync());
                    Assert.True(await client.IsEngineRunningAsync());
                }
            }

            await client.DisposeAsync();
            Assert.Equal(0, host.ExitCode);
            Assert.Contains("Managed editor protocol host completed", host.Output);
        }
        finally
        {
            Directory.Delete(workspace, recursive: true);
        }
    }
}
