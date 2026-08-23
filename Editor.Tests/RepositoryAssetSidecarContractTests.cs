using System.Diagnostics;
using SailorEditor.Content;
using YamlDotNet.RepresentationModel;

namespace Editor.Tests;

public sealed class RepositoryAssetSidecarContractTests
{
    static readonly HashSet<string> KnownAssetInfoTypes = new(StringComparer.Ordinal)
    {
        "Sailor::AssetInfo",
        "Sailor::AnimationAssetInfo",
        "Sailor::AudioAssetInfo",
        "Sailor::FrameGraphAssetInfo",
        "Sailor::MaterialAssetInfo",
        "Sailor::ModelAssetInfo",
        "Sailor::PrefabAssetInfo",
        "Sailor::ProbeVolumeAssetInfo",
        "Sailor::ShaderAssetInfo",
        "Sailor::TextureAssetInfo",
        "Sailor::WorldPrefabAssetInfo"
    };

    static readonly HashSet<string> RuntimeOrCacheFields = new(StringComparer.Ordinal)
    {
        "assetImportTime",
        "metadataLoadTime",
        "metadataPath"
    };

    [Fact]
    public void TrackedContentSidecars_ArePortableAndResolveSameDirectorySources()
    {
        var repositoryRoot = ResolveRepositoryRoot();
        var sidecars = EnumerateTrackedContentSidecars(repositoryRoot).ToArray();
        Assert.NotEmpty(sidecars);

        var violations = new List<string>();
        foreach (var relativePath in sidecars)
        {
            var metadataPath = Path.Combine(
                repositoryRoot,
                relativePath.Replace('/', Path.DirectorySeparatorChar));

            try
            {
                var yaml = new YamlStream();
                using var reader = File.OpenText(metadataPath);
                yaml.Load(reader);

                if (yaml.Documents.Count != 1 ||
                    yaml.Documents[0].RootNode is not YamlMappingNode root)
                {
                    violations.Add($"{relativePath}: expected one YAML mapping document.");
                    continue;
                }

                var forbiddenFields = EnumerateMappingKeys(root)
                    .Where(RuntimeOrCacheFields.Contains)
                    .Distinct(StringComparer.Ordinal)
                    .OrderBy(field => field, StringComparer.Ordinal)
                    .ToArray();
                if (forbiddenFields.Length != 0)
                {
                    violations.Add(
                        $"{relativePath}: contains runtime/cache fields: {string.Join(", ", forbiddenFields)}.");
                }

                if (!root.Children.TryGetValue(new YamlScalarNode("assetInfoType"), out var assetInfoTypeNode) ||
                    assetInfoTypeNode is not YamlScalarNode { Value: { } assetInfoType } ||
                    !KnownAssetInfoTypes.Contains(assetInfoType))
                {
                    violations.Add(
                        $"{relativePath}: assetInfoType must be one of: {string.Join(", ", KnownAssetInfoTypes.OrderBy(x => x, StringComparer.Ordinal))}.");
                }

                if (!root.Children.TryGetValue(new YamlScalarNode("filename"), out var filenameNode) ||
                    filenameNode is not YamlScalarNode { Value: { } declaredFilename })
                {
                    violations.Add($"{relativePath}: missing scalar filename.");
                    continue;
                }

                if (!AssetSourcePathContract.TryResolve(
                    metadataPath,
                    declaredFilename,
                    out _,
                    out var error))
                {
                    violations.Add($"{relativePath}: {error}");
                }
            }
            catch (Exception exception)
            {
                violations.Add($"{relativePath}: {exception.Message}");
            }
        }

        Assert.True(
            violations.Count == 0,
            "Invalid tracked Content asset sidecars:" + Environment.NewLine +
            string.Join(Environment.NewLine, violations));
    }

    static IEnumerable<string> EnumerateMappingKeys(YamlNode node)
    {
        if (node is YamlMappingNode mapping)
        {
            foreach (var entry in mapping.Children)
            {
                if (entry.Key is YamlScalarNode { Value: { } key })
                {
                    yield return key;
                }

                foreach (var nestedKey in EnumerateMappingKeys(entry.Value))
                {
                    yield return nestedKey;
                }
            }
        }
        else if (node is YamlSequenceNode sequence)
        {
            foreach (var child in sequence.Children)
            {
                foreach (var nestedKey in EnumerateMappingKeys(child))
                {
                    yield return nestedKey;
                }
            }
        }
    }

    static IEnumerable<string> EnumerateTrackedContentSidecars(string repositoryRoot)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = "git",
            WorkingDirectory = repositoryRoot,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("ls-files");
        startInfo.ArgumentList.Add("-z");
        startInfo.ArgumentList.Add("--");
        startInfo.ArgumentList.Add("Content");

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Could not start git ls-files.");
        var output = process.StandardOutput.ReadToEnd();
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();
        Assert.True(process.ExitCode == 0, $"git ls-files failed: {error}");

        return output
            .Split('\0', StringSplitOptions.RemoveEmptyEntries)
            .Where(path => path.EndsWith(".asset", StringComparison.OrdinalIgnoreCase))
            .Where(path => File.Exists(Path.Combine(
                repositoryRoot,
                path.Replace('/', Path.DirectorySeparatorChar))))
            .OrderBy(path => path, StringComparer.Ordinal)
            .ToArray();
    }

    static string ResolveRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "Content")) &&
                Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                Directory.Exists(Path.Combine(current.FullName, "Runtime")))
            {
                return current.FullName;
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
