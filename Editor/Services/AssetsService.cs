using SailorEditor.ViewModels;
using System.IO;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Services
{
    public class AssetsService
    {
        public ProjectRoot Root { get; private set; }
        public List<AssetFolder> Folders { get; private set; }
        public List<AssetFile> Files { get; private set; }
        public AssetsService() => AddProjectRoot(SailorEngine.GetEngineContentDirectory());
        public void AddProjectRoot(string projectRoot)
        {
            Folders = new List<AssetFolder>();
            Files = new List<AssetFile>();

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
                extension == ".bmp")
                newAssetFile = new TextureFile();
            else if (extension == ".shader")
                newAssetFile = new ShaderFile();
            else
                newAssetFile = new AssetFile();

            newAssetFile.Name = assetFile.Name;
            newAssetFile.Id = Files.Count + 1;
            newAssetFile.FolderId = parentFolderId;
            newAssetFile.AssetInfo = assetInfo;
            newAssetFile.Asset = assetFile;

            using (var yamlAssetInfo = new FileStream(assetInfo.FullName, FileMode.Open))
            using (var reader = new StreamReader(yamlAssetInfo))
            {
                var yaml = new YamlStream();
                yaml.Load(reader);

                var root = (YamlMappingNode)yaml.Documents[0].RootNode;

                foreach (var e in root.Children)
                {
                    newAssetFile.Properties[e.Key.ToString()] = e.Value.ToString();
                }

                yamlAssetInfo.Close();
            }

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
                    Files.Add(newAssetInfo);
                }
                catch (Exception ex)
                {
                    Console.WriteLine(ex.ToString());
                }
            }
        }
    }
}
