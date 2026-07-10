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
using SailorEditor.Content;

namespace SailorEditor.Services
{
    public class AssetsService
    {
        const int ActiveProjectRootId = 1;
        const int EngineProjectRootId = 2;

        readonly string _engineContentDirectory;
        ProjectContentFolderIdAllocator _folderIdAllocator = new();
        HashSet<string> _visitedDirectories = new(ProjectContentPathPolicy.PathComparer);
        int _nextAssetId = 1;
        int _assetOverrideCount;

        public event Action? Changed;

        public ProjectRoot Root { get; private set; }
        public List<AssetFolder> Folders { get; private set; }
        public Dictionary<FileId, AssetFile> Assets { get; private set; }
        public List<AssetFile> Files { get; private set; } = new();
        public string CurrentProjectRootPath { get; private set; }

        public AssetsService()
        {
            var engineContentDirectory = Path.GetFullPath(MauiProgram.GetService<EngineService>().EngineContentDirectory);
            Directory.CreateDirectory(engineContentDirectory);
            _engineContentDirectory = ProjectContentPathPolicy.NormalizeRoot(engineContentDirectory);
            AddProjectRoot(_engineContentDirectory);
        }

        public string GetFolderPath(AssetFolder? folder)
        {
            if (folder == null)
            {
                return CurrentProjectRootPath;
            }

            if (!string.IsNullOrWhiteSpace(folder.FullPath))
            {
                return folder.FullPath;
            }

            var parts = new Stack<string>();
            var current = folder;
            while (current != null)
            {
                parts.Push(current.Name);
                current = Folders.FirstOrDefault(x => x.Id == current.ParentFolderId);
            }

            return Path.Combine(CurrentProjectRootPath, Path.Combine(parts.ToArray()));
        }

        public PrefabFile? CreatePrefabAsset(AssetFolder? targetFolder, GameObject root, bool overwrite = false, PrefabFile? existingPrefab = null)
        {
            if ((targetFolder?.IsReadOnly ?? false) || (existingPrefab?.IsReadOnly ?? false))
                return null;

            var worldService = MauiProgram.GetService<WorldService>();
            var prefab = worldService.CreatePrefabFromSubHierarchy(root, out var externalRefs);
            var folderPath = existingPrefab?.Asset?.DirectoryName ?? GetFolderPath(targetFolder);
            if (!ProjectContentPathPolicy.IsInsideRoot(CurrentProjectRootPath, folderPath))
                return null;

            Directory.CreateDirectory(folderPath);

            var prefabName = existingPrefab?.Asset?.Name ?? GetUniqueAssetName(folderPath, root.Name, ".prefab");
            var prefabPath = existingPrefab?.Asset?.FullName ?? Path.Combine(folderPath, prefabName);
            var assetInfoPath = prefabPath + ".asset";
            var fileId = existingPrefab?.FileId ?? new FileId($"{{{Guid.NewGuid().ToString().ToUpperInvariant()}}}");

            WritePrefab(prefabPath, prefab);
            WritePrefabAssetInfo(assetInfoPath, fileId, Path.GetFileName(prefabPath), externalRefs);

            Refresh();
            var created = Assets.TryGetValue(fileId, out var asset) && asset is PrefabFile prefabFile
                ? prefabFile
                : null;

            return created;
        }

        public bool RenameAsset(AssetFile assetFile, string newName)
        {
            if (!CanModifyAsset(assetFile) || string.IsNullOrWhiteSpace(newName))
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

            if (!string.Equals(oldAssetPath, targetAssetPath, ProjectContentPathPolicy.PathComparison) && File.Exists(targetAssetPath))
            {
                return false;
            }

            if (!string.Equals(oldAssetInfoPath, targetAssetInfoPath, ProjectContentPathPolicy.PathComparison) && File.Exists(targetAssetInfoPath))
            {
                return false;
            }

            if (!string.IsNullOrEmpty(oldAssetPath) && File.Exists(oldAssetPath) && !string.Equals(oldAssetPath, targetAssetPath, ProjectContentPathPolicy.PathComparison))
            {
                File.Move(oldAssetPath, targetAssetPath);
            }

            if (!string.Equals(oldAssetInfoPath, targetAssetInfoPath, ProjectContentPathPolicy.PathComparison))
            {
                File.Move(oldAssetInfoPath, targetAssetInfoPath);
            }

            UpdateAssetInfoFilename(targetAssetInfoPath, targetAssetFileName);
            Refresh();
            return true;
        }

        public bool DeleteAsset(AssetFile assetFile)
        {
            if (!CanModifyAsset(assetFile))
                return false;

            try
            {
                if (assetFile.Asset?.Exists == true)
                    File.Delete(assetFile.Asset.FullName);
                if (assetFile.AssetInfo.Exists)
                    File.Delete(assetFile.AssetInfo.FullName);

                Refresh();
                return true;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[AssetsService] Failed to delete asset: {ex.Message}");
                return false;
            }
        }

        public void Refresh() => AddProjectRoot(CurrentProjectRootPath);

        public void AddProjectRoot(string projectRoot)
        {
            using var perfScope = EditorPerf.Scope("AssetsService.AddProjectRoot");

            Folders = [];
            Assets = [];
            Files = [];
            _folderIdAllocator = new ProjectContentFolderIdAllocator();
            _visitedDirectories = new HashSet<string>(ProjectContentPathPolicy.PathComparer);
            _nextAssetId = 1;
            _assetOverrideCount = 0;

            var requestedRoot = Path.GetFullPath(projectRoot);
            Directory.CreateDirectory(requestedRoot);
            CurrentProjectRootPath = ProjectContentPathPolicy.NormalizeRoot(requestedRoot);

            Root = new ProjectRoot { Name = CurrentProjectRootPath, Id = 1 };
            if (ProjectContentPathPolicy.IsSamePath(CurrentProjectRootPath, _engineContentDirectory))
            {
                ReadDirectory(Root, CurrentProjectRootPath, CurrentProjectRootPath, -1, useRootedFolderIds: false, isReadOnly: false);
            }
            else
            {
                var workspaceRoot = new ProjectRoot { Name = "Workspace Content", Id = ActiveProjectRootId };
                var engineRoot = new ProjectRoot { Name = "Engine Content", Id = EngineProjectRootId };

                AddContentRootFolder(workspaceRoot, ProjectContentFolderIds.WorkspaceContentRootId, CurrentProjectRootPath, isReadOnly: false);
                AddContentRootFolder(engineRoot, ProjectContentFolderIds.EngineContentRootId, _engineContentDirectory, isReadOnly: true);

                ReadDirectory(engineRoot, _engineContentDirectory, _engineContentDirectory, ProjectContentFolderIds.EngineContentRootId, useRootedFolderIds: true, isReadOnly: true);
                ReadDirectory(workspaceRoot, CurrentProjectRootPath, CurrentProjectRootPath, ProjectContentFolderIds.WorkspaceContentRootId, useRootedFolderIds: true, isReadOnly: false);
            }

            if (_assetOverrideCount > 0)
                Console.WriteLine($"[AssetsService] Resolved {_assetOverrideCount} duplicate asset IDs; workspace content takes precedence when present.");

            Changed?.Invoke();
        }

        void AddContentRootFolder(ProjectRoot root, int folderId, string rootPath, bool isReadOnly)
        {
            Folders.Add(new AssetFolder
            {
                ProjectRootId = root.Id,
                Name = root.Name,
                Id = folderId,
                ParentFolderId = -1,
                FullPath = rootPath,
                IsReadOnly = isReadOnly
            });
        }

        public bool CanModifyAsset(AssetFile? assetFile)
        {
            if (assetFile is null || assetFile.IsReadOnly || assetFile.AssetInfo is null)
                return false;

            if (!ProjectContentPathPolicy.IsInsideRoot(CurrentProjectRootPath, assetFile.AssetInfo.FullName))
                return false;

            return assetFile.Asset is null
                || ProjectContentPathPolicy.IsInsideRoot(CurrentProjectRootPath, assetFile.Asset.FullName);
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

        private AssetFile ReadAssetFile(FileInfo assetInfo, int parentFolderId, int projectRootId, bool isReadOnly)
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

            newAssetFile.Id = _nextAssetId++;
            newAssetFile.DisplayName = assetFile.Name;
            newAssetFile.FolderId = parentFolderId;
            newAssetFile.ProjectRootId = projectRootId;
            newAssetFile.AssetInfo = assetInfo;
            newAssetFile.Asset = assetFile;
            newAssetFile.AssetType = assetType;
            newAssetFile.IsDirty = false;
            newAssetFile.IsReadOnly = isReadOnly;

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

        private void ReadDirectory(
            ProjectRoot root,
            string rootPath,
            string directoryPath,
            int parentFolderId,
            bool useRootedFolderIds,
            bool isReadOnly)
        {
            var canonicalDirectoryPath = ProjectContentPathPolicy.NormalizeRoot(directoryPath);
            var visitKey = $"{root.Id}:{canonicalDirectoryPath}";
            if (!ProjectContentPathPolicy.IsInsideRoot(rootPath, canonicalDirectoryPath) || !_visitedDirectories.Add(visitKey))
                return;

            foreach (var directory in Directory.GetDirectories(directoryPath).Order(ProjectContentPathPolicy.PathComparer))
            {
                var canonicalChildPath = ProjectContentPathPolicy.NormalizeRoot(directory);
                if (!ProjectContentPathPolicy.IsInsideRoot(rootPath, canonicalChildPath)
                    || _visitedDirectories.Contains($"{root.Id}:{canonicalChildPath}"))
                    continue;

                var dirInfo = new DirectoryInfo(directory);
                var relativeDirectoryPath = Path.GetRelativePath(rootPath, directory);
                var folder = new AssetFolder
                {
                    ProjectRootId = root.Id,
                    Name = dirInfo.Name,
                    Id = _folderIdAllocator.Allocate(root.Id, relativeDirectoryPath, useRootedFolderIds),
                    ParentFolderId = parentFolderId,
                    FullPath = canonicalChildPath,
                    IsReadOnly = isReadOnly
                };

                Folders.Add(folder);

                ReadDirectory(root, rootPath, directory, folder.Id, useRootedFolderIds, isReadOnly);
            }

            foreach (var file in Directory.GetFiles(directoryPath).Order(ProjectContentPathPolicy.PathComparer))
            {
                if (!ProjectContentPathPolicy.IsInsideRoot(rootPath, file))
                    continue;

                var assetInfo = new FileInfo(file);
                if (!string.Equals(assetInfo.Extension, ".asset", StringComparison.OrdinalIgnoreCase))
                    continue;

                try
                {
                    var newAssetInfo = ReadAssetFile(assetInfo, parentFolderId, root.Id, isReadOnly);
                    TryPopulateAssetMetadataFromYaml(newAssetInfo);
                    if (newAssetInfo.FileId == null || newAssetInfo.FileId.IsEmpty())
                    {
                        Console.WriteLine($"[AssetsService] Skip asset with empty FileId: {assetInfo.FullName}");
                        continue;
                    }

                    Files.Add(newAssetInfo);
                    if (Assets.ContainsKey(newAssetInfo.FileId))
                    {
                        _assetOverrideCount++;
                        if (ProjectContentAssetResolutionPolicy.ShouldReplace(
                            Assets[newAssetInfo.FileId].IsReadOnly,
                            newAssetInfo.IsReadOnly))
                            Assets[newAssetInfo.FileId] = newAssetInfo;
                    }
                    else
                    {
                        Assets.Add(newAssetInfo.FileId, newAssetInfo);
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine(ex.ToString());
                }
            }
        }

    }
}
