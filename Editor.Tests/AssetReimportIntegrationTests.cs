using System.Diagnostics;
using System.Globalization;
using System.Text.Json.Nodes;
using SailorEditor.Protocol;
using SailorEditor.Workspace;
using YamlDotNet.RepresentationModel;
using Xunit.Abstractions;

namespace Editor.Tests;

public sealed class NativeEditorFactAttribute : FactAttribute
{
    public NativeEditorFactAttribute()
    {
        if (string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("SAILOR_NATIVE_TEST_HOST")))
            Skip = "Set SAILOR_NATIVE_TEST_HOST to the built VulkanSubmissionTests executable.";
    }
}

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
            await new WorkspaceManifestSerializer().SaveAsync(Path.Combine(workspace, "workspace.sailor"),
                WorkspaceManifest.CreateDefault("Editor reimport integration", engine));
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
                    geometry.Write(value);
                foreach (var index in new ushort[] { 0, 1, 2 }) geometry.Write(index);
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

            using var host = new NativeHost(workspace, engine, log);
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

            Assert.True(await client.ReimportAssetAsync(ModelId));
            var beforeLoad = ReadContent(content);
            var instance = await client.CreateModelInstanceAsync(ModelId, "Panel", "", false, null, "");
            Assert.True(instance.Succeeded);
            Assert.False(string.IsNullOrEmpty(instance.InstanceId));
            Assert.True(await client.UpdateAssetAsync(ModelId));
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
            Assert.True(await client.ReimportAssetAsync(ModelId));
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
            Assert.True(await client.UpdateAssetAsync(ModelId));
            Assert.Equal(0.1f, Number(ReadYaml(materialPath), "uniformsFloat", "material.transmissionFactor"));
            Assert.True(await client.ReimportAssetAsync(ModelId));
            Assert.Equal(0.8f, Number(ReadYaml(materialPath), "uniformsFloat", "material.transmissionFactor"));
            Assert.Equal(sourceBytes, await File.ReadAllBytesAsync(sourcePath));
            Assert.Equal(sourceTime, File.GetLastWriteTimeUtc(sourcePath));

            var valid = await File.ReadAllTextAsync(materialPath);
            await File.WriteAllTextAsync(materialPath, "uniformsFloat: [");
            Assert.False(await client.ReimportAssetAsync(ModelId));
            Assert.False(await client.ReimportAssetAsync(ModelId));
            await File.WriteAllTextAsync(materialPath, valid);
            Assert.True(await client.ReimportAssetAsync(ModelId));
            Assert.Equal(materialId, Scalar(ReadYaml(materialInfoPath), "fileId"));

            File.Delete(materialPath);
            Assert.True(await client.ReimportAssetAsync(ModelId));
            Assert.Equal(materialId, Scalar(ReadYaml(materialInfoPath), "fileId"));
            Assert.Equal(0.8f, Number(ReadYaml(materialPath), "uniformsFloat", "material.transmissionFactor"));
            var repaired = await File.ReadAllBytesAsync(materialPath);
            var repairedTime = File.GetLastWriteTimeUtc(materialPath);
            Assert.True(await client.ReimportAssetAsync(ModelId));
            Assert.Equal(repaired, await File.ReadAllBytesAsync(materialPath));
            Assert.Equal(repairedTime, File.GetLastWriteTimeUtc(materialPath));

            await client.DisposeAsync();
            Assert.Equal(0, host.ExitCode);
            Assert.Contains("Managed editor protocol host completed", host.Output);
        }
        finally
        {
            Directory.Delete(workspace, recursive: true);
        }
    }

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

    // The child calls the production native exports on its own main thread.
    // Only process bootstrap is adapted; protocol traffic uses the real socket.
    sealed class NativeHost(string workspace, string engine, ITestOutputHelper log) : ILocalEngineProtocolNativeBridge, IDisposable
    {
        readonly string directory = Directory.CreateDirectory(Path.Combine(workspace, "NativeHost")).FullName;
        Process? process;
        Task<string>? output;
        Task<string>? errors;

        public int ExitCode => process!.ExitCode;
        public string Output => (output?.GetAwaiter().GetResult() ?? "") + (errors?.GetAwaiter().GetResult() ?? "");

        public int StartLocalHost(byte[] requestData, ushort port, string authorizationToken)
        {
            if (process is not null)
            {
                if (!process.WaitForExit(15000))
                    throw new TimeoutException("The previous native host attempt did not finish.");
                process.Dispose();
            }
            File.Delete(Path.Combine(directory, "ready"));
            File.WriteAllBytes(Path.Combine(directory, "initialize.pb"), requestData);
            File.WriteAllText(Path.Combine(directory, "endpoint.yaml"), $"port: {port}\ntoken: {authorizationToken}\n");
            var start = new ProcessStartInfo(Path.GetFullPath(Environment.GetEnvironmentVariable("SAILOR_NATIVE_TEST_HOST")!))
            {
                WorkingDirectory = engine,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            start.ArgumentList.Add("--gpu-editor-protocol-host");
            start.ArgumentList.Add(directory);
            if (OperatingSystem.IsMacOS()) start.Environment["DYLD_PRINT_LIBRARIES"] = "1";
            process = Process.Start(start) ?? throw new InvalidOperationException("The native test host did not start.");
            output = process.StandardOutput.ReadToEndAsync();
            errors = process.StandardError.ReadToEndAsync();
            var ready = Path.Combine(directory, "ready");
            var deadline = Stopwatch.StartNew();
            while (!File.Exists(ready) && !process.HasExited && deadline.Elapsed < TimeSpan.FromSeconds(60))
                Thread.Sleep(10);
            if (File.Exists(ready)) return int.Parse(File.ReadAllText(ready), CultureInfo.InvariantCulture);
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            process.WaitForExit();
            throw new InvalidOperationException($"Native editor host did not initialize.\n{Output}");
        }

        public void RequestLocalHostStop() => File.WriteAllText(Path.Combine(directory, "stop"), "");

        public void StopLocalHost(bool shutdownEngine)
        {
            RequestLocalHostStop();
            if (process is null) return;
            if (!process.WaitForExit(15000))
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit();
                throw new TimeoutException($"Native editor host did not shut down.\n{Output}");
            }
            if (process.ExitCode != 0) throw new InvalidOperationException($"Native editor host failed.\n{Output}");
        }

        public void Dispose()
        {
            try { if (process is not null && !process.HasExited) StopLocalHost(true); }
            finally
            {
                try { log.WriteLine(Output); }
                finally { process?.Dispose(); }
            }
        }
    }
}
