using SailorEditor.ViewModels;
using System.IO;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using Microsoft.Maui.Layouts;

namespace SailorEditor.Services
{
    public class AssetsService
    { 
        public ProjectRoot Root { get; private set; }
        public List<AssetFolder> Folders { get; private set; }
        public Dictionary<AssetUID, AssetFile> Assets { get; private set; }
        public List<AssetFile> Files { get { return Assets.Values.ToList(); } }
        public AssetsService() => AddProjectRoot(SailorEngine.GetEngineContentDirectory());
        public void AddProjectRoot(string projectRoot)
        {
            Folders = new List<AssetFolder>();
            Assets = new Dictionary<AssetUID, AssetFile>();

            Root = new ProjectRoot { Name = projectRoot, Id = 1 };
            ProcessDirectory(Root, projectRoot, -1);
        }
        private AssetFile ProcessAssetFile(FileInfo assetInfo, int parentFolderId)
        {
            FileInfo assetFile = new FileInfo(Path.ChangeExtension(assetInfo.FullName, null));

            AssetFile newAssetFile;

            var extension = Path.GetExtension(assetFile.FullName);
            if (extension == ".png" ||
                extension == ".jpg" ||
                extension == ".bmp" ||
                extension == ".dds" ||
                extension == ".hdr")
                newAssetFile = new TextureFile();
            else if (extension == ".obj" ||
                extension == ".fbx" ||
                extension == ".gltf")
                newAssetFile = new ModelFile();
            else if (extension == ".shader")
                newAssetFile = new ShaderFile();
            else if (extension == ".mat")
                newAssetFile = new MaterialFile();
            else if (extension == ".glsl")
                newAssetFile = new ShaderLibraryFile();
            else
                newAssetFile = new AssetFile();

            newAssetFile.Id = Files.Count + 1;
            newAssetFile.DisplayName = assetFile.Name;
            newAssetFile.FolderId = parentFolderId;
            newAssetFile.AssetInfo = assetInfo;
            newAssetFile.Asset = assetFile;            
            newAssetFile.Properties = ParseYaml(assetInfo.FullName);
            newAssetFile.IsDirty = false;

            return newAssetFile;
        }
        private void ProcessDirectory(ProjectRoot root, string directoryPath, int parentFolderId)
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

                ProcessDirectory(root, directory, folder.Id);
            }

            foreach (var file in Directory.GetFiles(directoryPath))
            {
                var assetInfo = new FileInfo(file);
                if (assetInfo.Extension != ".asset")
                    continue;

                try
                {
                    var newAssetInfo = ProcessAssetFile(assetInfo, parentFolderId);
                    Assets[newAssetInfo.UID] = newAssetInfo;
                }
                catch (Exception ex)
                {
                    Console.WriteLine(ex.ToString());
                }
            }
        }

        public static Dictionary<string, object> ParseYaml(string filename)
        {
            Dictionary<string, object> res = new Dictionary<string, object>();
            using (var yamlAssetInfo = new FileStream(filename, FileMode.Open))
            using (var reader = new StreamReader(yamlAssetInfo))
            {
                var yaml = new YamlStream();
                yaml.Load(reader);

                var root = (YamlMappingNode)yaml.Documents[0].RootNode;

                foreach (var e in root.Children)
                {
                    res[e.Key.ToString()] = e.Value;
                }

                yamlAssetInfo.Close();
            }
            return res;
        }
    }
}
