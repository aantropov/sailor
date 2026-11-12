using System.Text.Json;
using System.IO.Compression;
using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class GameBuildNativeFactAttribute : FactAttribute
{
    public GameBuildNativeFactAttribute()
    {
        if (string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("SAILOR_GAME_EXPORT_ENGINE")))
        {
            Skip = "Set SAILOR_GAME_EXPORT_ENGINE and SAILOR_GAME_EXPORT_BUILD to validate a real configured engine build.";
        }
    }
}

public sealed class GameBuildNativeTests
{
    [GameBuildNativeFact]
    public async Task GeneratedWorkspaceBuildsThroughSdkAndRelocatesWithoutAuthoringFiles()
    {
        var engine = Path.GetFullPath(Environment.GetEnvironmentVariable("SAILOR_GAME_EXPORT_ENGINE")!);
        var build = Path.GetFullPath(Environment.GetEnvironmentVariable("SAILOR_GAME_EXPORT_BUILD")
            ?? throw new InvalidOperationException("SAILOR_GAME_EXPORT_BUILD is required."));
        var evidenceParent = Environment.GetEnvironmentVariable("SAILOR_GAME_EXPORT_EVIDENCE");
        var root = Path.Combine(evidenceParent ?? Path.GetTempPath(), "sailor-export-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var workspace = Path.Combine(root, "project Я");
            var session = await new WorkspaceTemplateService(new WorkspaceManifestSerializer())
                .CreateAsync(new WorkspaceCreateRequest("Export integration", workspace, engine));
            await File.WriteAllTextAsync(Path.Combine(session.ContentDirectory, "marker Я.txt"), "project-owned content");
            await File.WriteAllTextAsync(Path.Combine(session.ContentDirectory, "model Я.gltf"),
                "{\"asset\":{\"version\":\"2.0\"},\"materials\":[{\"name\":\"Export test\"}]}");
            var packaging = Enum.Parse<GameAssetPackaging>(Environment.GetEnvironmentVariable("SAILOR_GAME_EXPORT_ASSETS") ?? "Zip");
            var configuration = Environment.GetEnvironmentVariable("SAILOR_GAME_EXPORT_CONFIGURATION") ?? "Release";
            var request = new GameBuildRequest(workspace, engine, Path.Combine(root, "build"), configuration,
                GameBuildSettings.HostPlatform, new() { Assets = packaging, Compression = GameArchiveCompression.Stored })
            {
                EngineBuildDirectory = build,
                BuildRuntime = false
            };
            var runner = new WorkspaceProcessRunner();
            using var log = new StreamWriter(Path.Combine(root, "export.log")) { AutoFlush = true };
            var result = await new GameBuildService(runner).BuildAsync(request, new ExportProgress(log));
            Assert.True(result.Succeeded, result.Error + "\nEvidence: " + root);
            Assert.False(File.Exists(Path.Combine(session.ContentDirectory, "model Я.gltf.asset")));
            var exportedFiles = packaging == GameAssetPackaging.Zip ? ReadArchiveFiles(Path.Combine(result.OutputDirectory, "Content.zip")) :
                Directory.GetFiles(Path.Combine(result.OutputDirectory, "Content"), "*", SearchOption.AllDirectories)
                    .Select(path => Path.GetRelativePath(Path.Combine(result.OutputDirectory, "Content"), path)).ToArray();
            Assert.Contains("model Я.gltf.asset", exportedFiles);
            Assert.Contains(exportedFiles, path => path.EndsWith(".mat", StringComparison.Ordinal));
            var relocated = WorkspacePathPolicy.NormalizePhysicalPath(Path.Combine(root, "relocated game Я"));
            Directory.Move(result.OutputDirectory, relocated);
            Directory.Move(workspace, workspace + ".hidden");
            Assert.False(Directory.Exists(Path.Combine(relocated, "Source")));
            Assert.False(Directory.Exists(Path.Combine(relocated, "Generated")));
            Assert.False(Directory.Exists(Path.Combine(relocated, "Cache")));
            var executable = Path.Combine(relocated, "bin", "SailorEngine-" + configuration + (OperatingSystem.IsWindows() ? ".exe" : ""));
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
            var describe = await runner.RunAsync(new(executable, ["--describe-workspace", relocated], root), timeout.Token);
            Assert.True(describe.Succeeded, describe.Output);
            using var description = JsonDocument.Parse(describe.Output);
            Assert.Equal(session.Manifest.WorkspaceId, description.RootElement.GetProperty("workspaceId").GetString());
            Assert.True(WorkspacePathPolicy.IsInsideRoot(relocated, description.RootElement.GetProperty("engineContent").GetString()!));
            Assert.True(WorkspacePathPolicy.IsInsideRoot(relocated, description.RootElement.GetProperty("logicOutput").GetString()!));
            await File.WriteAllTextAsync(Path.Combine(root, "results.json"), JsonSerializer.Serialize(new
            {
                schemaVersion = 1, engine, build, configuration, workspace = workspace + ".hidden", package = relocated,
                session.Manifest.WorkspaceId, packaging = packaging.ToString(), runtimeDescription = description.RootElement.Clone()
            }, new JsonSerializerOptions { WriteIndented = true }));
        }
        finally
        {
            if (evidenceParent is null)
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    static string[] ReadArchiveFiles(string path)
    {
        using var archive = ZipFile.OpenRead(path);
        return archive.Entries.Select(entry => entry.FullName).ToArray();
    }

    sealed class ExportProgress(TextWriter log) : IProgress<string>
    {
        public void Report(string value) => log.WriteLine(value);
    }
}
