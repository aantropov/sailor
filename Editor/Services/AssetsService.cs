using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using System.Collections.Generic;

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

            AssetFile newAssetFile = extension switch
            {
                ".png" or ".jpg" or ".tga" or ".bmp" or ".dds" or ".hdr" => new TextureFile(),
                ".obj" or ".gltf" or ".glb" => new ModelFile(),
                ".shader" => new ShaderFile(),
                ".mat" => new MaterialFile(),
                ".glsl" => new ShaderLibraryFile(),
                _ => new AssetFile()
            };

            newAssetFile.Id = Files.Count + 1;
            newAssetFile.DisplayName = assetFile.Name;
            newAssetFile.FolderId = parentFolderId;
            newAssetFile.AssetInfo = assetInfo;
            newAssetFile.Asset = assetFile;
            newAssetFile.IsDirty = false;

            _ = newAssetFile.Revert();

            // Resolve glb
            if (newAssetFile is TextureFile)
            {
                newAssetFile.Asset = new(assetInfo.Directory + $"/{(newAssetFile as TextureFile).Filename.Value.ToString()}");
            }

            return newAssetFile;
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
