using Editor.Models;

namespace Editor.Services
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

        private void ProcessDirectory(ProjectRoot root, string directoryPath, int parentFolderId)
        {
            foreach (var directory in Directory.GetDirectories(directoryPath))
            {
                var dirInfo = new DirectoryInfo(directory);
                var folder = new AssetFolder
                {
                    CompanyId = root.Id,
                    Name = dirInfo.Name,
                    Id = Folders.Count + 1,
                    ParentFolderId = parentFolderId
                };

                Folders.Add(folder);

                ProcessDirectory(root, directory, folder.Id);
            }

            foreach (var file in Directory.GetFiles(directoryPath))
            {
                var fileInfo = new FileInfo(file);
                Files.Add(new AssetFile
                {
                    Name = fileInfo.Name,
                    Id = Files.Count + 1,
                    FolderId = parentFolderId
                });
            }
        }
    }
}
