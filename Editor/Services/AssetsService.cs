using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using System.Collections.Generic;
using YamlDotNet.Core;

namespace SailorEditor.Services
{
    public class AssetsService
    {
        public ProjectRoot Root { get; private set; }
        public List<AssetFolder> Folders { get; private set; }
        public Dictionary<FileId, AssetFile> Assets { get; private set; }
        public List<AssetFile> Files { get; private set; } = new();

        public AssetsService() => AddProjectRoot(MauiProgram.GetService<EngineService>().EngineContentDirectory);

        public void AddProjectRoot(string projectRoot)
        {
            Folders = [];
            Assets = [];

            Root = new ProjectRoot { Name = projectRoot, Id = 1 };
            ReadDirectory(Root, projectRoot, -1);

            Files = new HashSet<AssetFile>([.. Assets.Values]).ToList();
        }

        private AssetFile ReadAssetFile(FileInfo assetInfo, int parentFolderId)
        {
            FileInfo assetFile = new(Path.ChangeExtension(assetInfo.FullName, null));

            var extension = Path.GetExtension(assetFile.FullName);
            var assetInfoType = TryReadScalar(assetInfo, "assetInfoType");

            AssetFile newAssetFile = assetInfoType switch
            {
                "Sailor::TextureAssetInfo" => new TextureFile(),
                "Sailor::ModelAssetInfo" => new ModelFile(),
                "Sailor::AnimationAssetInfo" => new AnimationFile(),
                "Sailor::PrefabAssetInfo" => new PrefabFile(),
                "Sailor::WorldPrefabAssetInfo" => new WorldFile(),
                "Sailor::ShaderAssetInfo" => new ShaderFile(),
                "Sailor::MaterialAssetInfo" => new MaterialFile(),
                "Sailor::FrameGraphAssetInfo" => new FrameGraphFile(),
                _ => CreateAssetFileByExtension(extension)
            };

            newAssetFile.AssetInfoTypeName = assetInfoType;

            var engineTypes = MauiProgram.GetService<EngineService>()?.EngineTypes;
            AssetType assetType = null;
            if (!string.IsNullOrEmpty(assetInfoType))
            {
                engineTypes?.AssetTypes?.TryGetValue(assetInfoType, out assetType);
            }

            if (assetType == null)
            {
                engineTypes?.AssetTypesByExtension?.TryGetValue(extension.TrimStart('.').ToLowerInvariant(), out assetType);
            }

            newAssetFile.Id = Files.Count + 1;
            newAssetFile.DisplayName = assetFile.Name;
            newAssetFile.FolderId = parentFolderId;
            newAssetFile.AssetInfo = assetInfo;
            newAssetFile.Asset = assetFile;
            newAssetFile.AssetType = assetType;
            newAssetFile.IsDirty = false;

            _ = newAssetFile.Revert();

            // Resolve glb
            if (newAssetFile is TextureFile)
            {
                newAssetFile.Asset = new(assetInfo.Directory + $"/{(newAssetFile as TextureFile).Filename.Value.ToString()}");
            }

            return newAssetFile;
        }

        private static AssetFile CreateAssetFileByExtension(string extension) => extension switch
        {
            ".png" or ".jpg" or ".tga" or ".bmp" or ".dds" or ".hdr" => new TextureFile(),
            ".obj" or ".gltf" or ".glb" => new ModelFile(),
            ".anim" => new AnimationFile(),
            ".prefab" => new PrefabFile(),
            ".world" => new WorldFile(),
            ".shader" => new ShaderFile(),
            ".mat" => new MaterialFile(),
            ".renderer" => new FrameGraphFile(),
            ".glsl" => new ShaderLibraryFile(),
            _ => new AssetFile()
        };

        private static string TryReadScalar(FileInfo assetInfo, string fieldName)
        {
            try
            {
                using var reader = new StreamReader(assetInfo.FullName);
                var yaml = new YamlStream();
                yaml.Load(reader);
                if (yaml.Documents.Count > 0 &&
                    yaml.Documents[0].RootNode is YamlMappingNode root &&
                    root.Children.TryGetValue(new YamlScalarNode(fieldName), out var node))
                {
                    return node?.ToString();
                }
            }
            catch (YamlException ex)
            {
                Console.WriteLine($"[AssetsService] Failed to read {fieldName}: {assetInfo.FullName} :: {ex.Message}");
            }

            return null;
        }

        private void TryPopulateAssetMetadataFromYaml(AssetFile assetFile)
        {
            if (assetFile.AssetInfo == null || !assetFile.AssetInfo.Exists)
            {
                return;
            }

            if (assetFile.FileId != null && !assetFile.FileId.IsEmpty() && assetFile.Filename != null && !assetFile.Filename.IsEmpty())
            {
                return;
            }

            try
            {
                using var reader = new StreamReader(assetFile.AssetInfo.FullName);
                var yaml = new YamlStream();
                yaml.Load(reader);
                if (yaml.Documents.Count == 0 || yaml.Documents[0].RootNode is not YamlMappingNode root)
                {
                    return;
                }

                if (string.IsNullOrEmpty(assetFile.AssetInfoTypeName) && root.Children.TryGetValue(new YamlScalarNode("assetInfoType"), out var assetInfoTypeNode))
                {
                    assetFile.AssetInfoTypeName = assetInfoTypeNode?.ToString();
                }

                if ((assetFile.FileId == null || assetFile.FileId.IsEmpty()) && root.Children.TryGetValue(new YamlScalarNode("fileId"), out var fileIdNode))
                {
                    var value = fileIdNode?.ToString();
                    if (!string.IsNullOrWhiteSpace(value))
                    {
                        assetFile.FileId = new FileId(value);
                    }
                }

                if ((assetFile.Filename == null || assetFile.Filename.IsEmpty()) && root.Children.TryGetValue(new YamlScalarNode("filename"), out var filenameNode))
                {
                    var value = filenameNode?.ToString();
                    if (!string.IsNullOrWhiteSpace(value))
                    {
                        assetFile.Filename = new FileId(value);
                    }
                }
            }
            catch (YamlException ex)
            {
                Console.WriteLine($"[AssetsService] Failed to parse metadata from YAML: {assetFile.AssetInfo.FullName} :: {ex.Message}");
            }
        }

        private void ReadDirectory(ProjectRoot root, string directoryPath, int parentFolderId)
        {
            foreach (var directory in Directory.GetDirectories(directoryPath))
            {
                var dirInfo = new DirectoryInfo(directory);
                var folder = new AssetFolder
                {
                    ProjectRootId = root.Id,
                    Name = dirInfo.Name,
                    Id = Folders.Count + 1,
                    ParentFolderId = parentFolderId
                };

                Folders.Add(folder);

                ReadDirectory(root, directory, folder.Id);
            }

            foreach (var file in Directory.GetFiles(directoryPath))
            {
                var assetInfo = new FileInfo(file);
                if (assetInfo.Extension != ".asset")
                    continue;

                try
                {
                    var newAssetInfo = ReadAssetFile(assetInfo, parentFolderId);
                    TryPopulateAssetMetadataFromYaml(newAssetInfo);
                    if (newAssetInfo.FileId == null || newAssetInfo.FileId.IsEmpty())
                    {
                        Console.WriteLine($"[AssetsService] Skip asset with empty FileId: {assetInfo.FullName}");
                        continue;
                    }

                    Assets[newAssetInfo.FileId] = newAssetInfo;
                }
                catch (Exception ex)
                {
                    Console.WriteLine(ex.ToString());
                }
            }
        }
    }
}
