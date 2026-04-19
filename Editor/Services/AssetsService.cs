using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using System.Collections.Generic;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using SailorEditor.Helpers;

namespace SailorEditor.Services
{
    public class AssetsService
    {
        public event Action? Changed;

        public ProjectRoot Root { get; private set; }
        public List<AssetFolder> Folders { get; private set; }
        public Dictionary<FileId, AssetFile> Assets { get; private set; }
        public List<AssetFile> Files { get; private set; } = new();

        public AssetsService() => AddProjectRoot(MauiProgram.GetService<EngineService>().EngineContentDirectory);

        public string GetFolderPath(AssetFolder folder)
        {
            if (folder == null)
            {
                return MauiProgram.GetService<EngineService>().EngineContentDirectory;
            }

            var parts = new Stack<string>();
            var current = folder;
            while (current != null)
            {
                parts.Push(current.Name);
                current = Folders.FirstOrDefault(x => x.Id == current.ParentFolderId);
            }

            return Path.Combine(MauiProgram.GetService<EngineService>().EngineContentDirectory, Path.Combine(parts.ToArray()));
        }

        public PrefabFile CreatePrefabAsset(AssetFolder targetFolder, GameObject root, bool overwrite = false, PrefabFile existingPrefab = null)
        {
            var worldService = MauiProgram.GetService<WorldService>();
            var prefab = worldService.CreatePrefabFromSubHierarchy(root, out var externalRefs);
            var folderPath = existingPrefab?.Asset?.DirectoryName ?? GetFolderPath(targetFolder);
            Directory.CreateDirectory(folderPath);

            var prefabName = existingPrefab?.Asset?.Name ?? GetUniqueAssetName(folderPath, root.Name, ".prefab");
            var prefabPath = existingPrefab?.Asset?.FullName ?? Path.Combine(folderPath, prefabName);
            var assetInfoPath = prefabPath + ".asset";
            var fileId = existingPrefab?.FileId ?? new FileId($"{{{Guid.NewGuid().ToString().ToUpperInvariant()}}}");

            WritePrefab(prefabPath, prefab);
            WritePrefabAssetInfo(assetInfoPath, fileId, Path.GetFileName(prefabPath), externalRefs);

            AddProjectRoot(MauiProgram.GetService<EngineService>().EngineContentDirectory);
            var created = Assets.TryGetValue(fileId, out var asset) && asset is PrefabFile prefabFile
                ? prefabFile
                : null;

            return created;
        }

        public bool RenameAsset(AssetFile assetFile, string newName)
        {
            if (assetFile?.AssetInfo == null || string.IsNullOrWhiteSpace(newName))
            {
                return false;
            }

            var sanitizedName = string.Join("_", newName.Trim().Split(Path.GetInvalidFileNameChars(), StringSplitOptions.RemoveEmptyEntries));
            if (string.IsNullOrWhiteSpace(sanitizedName))
            {
                return false;
            }

            var oldAssetPath = assetFile.Asset?.FullName;
            var oldAssetInfoPath = assetFile.AssetInfo.FullName;
            var directory = Path.GetDirectoryName(oldAssetInfoPath);
            var oldExtension = assetFile.Asset?.Extension ?? Path.GetExtension(Path.ChangeExtension(oldAssetInfoPath, null));
            var requestedExtension = Path.GetExtension(sanitizedName);
            var targetAssetFileName = string.IsNullOrEmpty(requestedExtension) ? sanitizedName + oldExtension : sanitizedName;
            var targetAssetPath = Path.Combine(directory, targetAssetFileName);
            var targetAssetInfoPath = targetAssetPath + ".asset";

            if (!string.Equals(oldAssetPath, targetAssetPath, StringComparison.OrdinalIgnoreCase) && File.Exists(targetAssetPath))
            {
                return false;
            }

            if (!string.Equals(oldAssetInfoPath, targetAssetInfoPath, StringComparison.OrdinalIgnoreCase) && File.Exists(targetAssetInfoPath))
            {
                return false;
            }

            if (!string.IsNullOrEmpty(oldAssetPath) && File.Exists(oldAssetPath) && !string.Equals(oldAssetPath, targetAssetPath, StringComparison.OrdinalIgnoreCase))
            {
                File.Move(oldAssetPath, targetAssetPath);
            }

            if (!string.Equals(oldAssetInfoPath, targetAssetInfoPath, StringComparison.OrdinalIgnoreCase))
            {
                File.Move(oldAssetInfoPath, targetAssetInfoPath);
            }

            UpdateAssetInfoFilename(targetAssetInfoPath, targetAssetFileName);
            AddProjectRoot(MauiProgram.GetService<EngineService>().EngineContentDirectory);
            return true;
        }

        public void AddProjectRoot(string projectRoot)
        {
            Folders = [];
            Assets = [];

            Root = new ProjectRoot { Name = projectRoot, Id = 1 };
            ReadDirectory(Root, projectRoot, -1);

            Files = new HashSet<AssetFile>([.. Assets.Values]).ToList();
            Changed?.Invoke();
        }

        static string GetUniqueAssetName(string folderPath, string baseName, string extension)
        {
            var sanitized = string.Join("_", baseName.Split(Path.GetInvalidFileNameChars(), StringSplitOptions.RemoveEmptyEntries));
            if (string.IsNullOrWhiteSpace(sanitized))
            {
                sanitized = "Prefab";
            }

            var fileName = sanitized + extension;
            var index = 1;
            while (File.Exists(Path.Combine(folderPath, fileName)) || File.Exists(Path.Combine(folderPath, fileName + ".asset")))
            {
                fileName = $"{sanitized}_{index++}{extension}";
            }

            return fileName;
        }

        static void WritePrefab(string path, Prefab prefab)
        {
            var commonConverters = new List<IYamlTypeConverter> {
                new ComponentTypeYamlConverter(),
                new ViewModels.ComponentYamlConverter()
            };

            var serializerBuilder = SerializationUtils.CreateSerializerBuilder()
                .WithTypeConverter(new ObservableListConverter<GameObject>(commonConverters.ToArray()))
                .WithTypeConverter(new ObservableListConverter<Component>(commonConverters.ToArray()));

            commonConverters.ForEach(converter => serializerBuilder.WithTypeConverter(converter));

            var serializer = serializerBuilder.Build();
            File.WriteAllText(path, serializer.Serialize(prefab));
        }

        static void WritePrefabAssetInfo(string path, FileId fileId, string filename, List<InstanceId> externalRefs)
        {
            var root = new YamlMappingNode
            {
                { "assetInfoType", "Sailor::PrefabAssetInfo" },
                { "fileId", fileId.Value },
                { "filename", filename }
            };

            if (externalRefs.Count > 0)
            {
                var warnings = new YamlSequenceNode();
                warnings.Add($"Prefab contains scene references: {string.Join(", ", externalRefs.Select(x => x.Value))}");
                root.Add("warnings", warnings);
            }

            var yaml = new YamlStream(new YamlDocument(root));
            using var writer = new StreamWriter(new FileStream(path, FileMode.Create));
            yaml.Save(writer, false);
        }

        static void UpdateAssetInfoFilename(string assetInfoPath, string filename)
        {
            var yaml = new YamlStream();
            using (var reader = new StreamReader(assetInfoPath))
            {
                yaml.Load(reader);
            }

            YamlMappingNode root;
            if (yaml.Documents.Count == 0 || yaml.Documents[0].RootNode is not YamlMappingNode mapping)
            {
                root = new YamlMappingNode();
                yaml = new YamlStream(new YamlDocument(root));
            }
            else
            {
                root = mapping;
            }

            root.Children[new YamlScalarNode("filename")] = new YamlScalarNode(filename);

            using var writer = new StreamWriter(new FileStream(assetInfoPath, FileMode.Create));
            yaml.Save(writer, false);
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
