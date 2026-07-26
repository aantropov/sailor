using System.Globalization;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Content;

public sealed record ProjectContentFileOperationResult(
    bool Succeeded,
    string? Error,
    bool RollbackSucceeded,
    IReadOnlyList<string> UnrestoredPaths,
    string? CreatedAssetInfoPath = null,
    string? CreatedFileId = null);

public interface IProjectContentFileSystem
{
    bool FileExists(string path);
    bool DirectoryExists(string path);
    IEnumerable<string> EnumerateFiles(string directoryPath);
    string ReadAllText(string path);
    void WriteAllText(string path, string contents);
    void WriteAllTextNew(string path, string contents);
    void CreateDirectory(string path);
    void CopyFile(string sourcePath, string destinationPath);
    void MoveFile(string sourcePath, string destinationPath);
    void DeleteFile(string path);
    void MoveDirectory(string sourcePath, string destinationPath);
    void DeleteDirectory(string path, bool recursive);
}

public sealed class PhysicalProjectContentFileSystem : IProjectContentFileSystem
{
    public static PhysicalProjectContentFileSystem Instance { get; } = new();

    PhysicalProjectContentFileSystem()
    {
    }

    public bool FileExists(string path) => File.Exists(path);
    public bool DirectoryExists(string path) => Directory.Exists(path);
    public IEnumerable<string> EnumerateFiles(string directoryPath) => Directory.EnumerateFiles(directoryPath);
    public string ReadAllText(string path) => File.ReadAllText(path);
    public void WriteAllText(string path, string contents) => File.WriteAllText(path, contents);
    public void WriteAllTextNew(string path, string contents)
    {
        using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        using var writer = new StreamWriter(stream, new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        writer.Write(contents);
    }
    public void CreateDirectory(string path) => Directory.CreateDirectory(path);
    public void CopyFile(string sourcePath, string destinationPath) => File.Copy(sourcePath, destinationPath);
    public void MoveFile(string sourcePath, string destinationPath) => File.Move(sourcePath, destinationPath);
    public void DeleteFile(string path) => File.Delete(path);
    public void MoveDirectory(string sourcePath, string destinationPath) => Directory.Move(sourcePath, destinationPath);
    public void DeleteDirectory(string path, bool recursive) => Directory.Delete(path, recursive);
}

public sealed class ProjectContentFileOperations
{
    static readonly HashSet<string> ReservedPortableNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    const string PortableInvalidFilenameCharacters = "<>:\"/\\|?*";

    readonly IProjectContentFileSystem fileSystem;
    readonly object mutationLock = new();

    public ProjectContentFileOperations(IProjectContentFileSystem? fileSystem = null)
    {
        this.fileSystem = fileSystem ?? PhysicalProjectContentFileSystem.Instance;
    }

    public ProjectContentFileOperationResult MoveAssetGroup(
        string writableRoot,
        string assetInfoPath,
        string destinationDirectory)
    {
        lock (mutationLock)
        {
            if (!TryResolveAssetGroup(writableRoot, assetInfoPath, out var root, out var group, out var error))
                return Failure(error);

            if (!TryResolveDestinationDirectory(root, destinationDirectory, out var destination, out error))
                return Failure(error);

            var sourceDirectory = Path.GetDirectoryName(group.SourcePath)!;
            if (ProjectContentPathPolicy.IsSamePath(sourceDirectory, destination))
                return Failure("The asset group is already in the destination directory.");

            var moves = EnumerateAssetGroupPaths(group)
                .Select(sourcePath => new FileMove(
                    sourcePath,
                    Path.Combine(destination, Path.GetFileName(sourcePath))))
                .ToArray();

            if (!TryPreflightFileMoves(root, moves, out error))
                return Failure(error);

            return ExecuteFileMoves(moves, "move the asset group");
        }
    }

    public ProjectContentFileOperationResult DuplicateAssetGroup(
        string writableRoot,
        string assetInfoPath)
    {
        lock (mutationLock)
        {
            if (!TryResolveAssetGroup(writableRoot, assetInfoPath, out var root, out var group, out var error))
                return Failure(error);

            var primaryAssetInfoPath = group.SourcePath + ".asset";
            if (!group.AssetInfoPaths.Any(path => ProjectContentPathPolicy.IsSamePath(path, primaryAssetInfoPath)))
                return Failure("The asset group has no primary sidecar beside its source file.");

            if (!TryPrepareDuplicateAssetGroup(
                root,
                group,
                primaryAssetInfoPath,
                out var plan,
                out error))
            {
                return Failure(error);
            }

            return ExecuteDuplicate(plan);
        }
    }

    public ProjectContentFileOperationResult RenameAssetGroup(
        string writableRoot,
        string assetInfoPath,
        string newName)
    {
        lock (mutationLock)
        {
            if (!TryResolveAssetGroup(writableRoot, assetInfoPath, out var root, out var group, out var error))
                return Failure(error);

            if (!TryNormalizePortableBasename(newName, out var requestedName, out error))
                return Failure(error);

            var currentExtension = Path.GetExtension(group.SourcePath);
            var targetFilename = string.IsNullOrEmpty(Path.GetExtension(requestedName))
                ? requestedName + currentExtension
                : requestedName;
            if (!TryNormalizePortableBasename(targetFilename, out targetFilename, out error))
                return Failure(error);

            var sourceDirectory = Path.GetDirectoryName(group.SourcePath)!;
            var targetSourcePath = Path.Combine(sourceDirectory, targetFilename);
            if (ProjectContentPathPolicy.IsSamePath(group.SourcePath, targetSourcePath))
                return Failure("The asset already has that name.");

            var primaryAssetInfoPath = group.SourcePath + ".asset";
            if (!group.AssetInfoPaths.Any(path => ProjectContentPathPolicy.IsSamePath(path, primaryAssetInfoPath)))
                return Failure("The asset group has no primary sidecar beside its source file.");

            var targetPrimaryAssetInfoPath = targetSourcePath + ".asset";
            var moves = new[]
            {
                new FileMove(group.SourcePath, targetSourcePath),
                new FileMove(primaryAssetInfoPath, targetPrimaryAssetInfoPath)
            };

            if (!TryPreflightFileMoves(root, moves, out error))
                return Failure(error);

            var rewrites = new List<MetadataRewrite>(group.AssetInfoPaths.Count);
            foreach (var metadataPath in group.AssetInfoPaths.OrderBy(path => path, StringComparer.Ordinal))
            {
                string originalContents;
                string updatedContents;
                try
                {
                    originalContents = fileSystem.ReadAllText(metadataPath);
                    if (!TryRewriteDeclaredFilename(originalContents, targetFilename, out updatedContents, out error))
                        return Failure($"Cannot update asset metadata '{metadataPath}': {error}");
                }
                catch (Exception exception)
                {
                    return Failure($"Cannot read asset metadata '{metadataPath}': {exception.Message}");
                }

                var finalPath = ProjectContentPathPolicy.IsSamePath(metadataPath, primaryAssetInfoPath)
                    ? targetPrimaryAssetInfoPath
                    : metadataPath;
                rewrites.Add(new MetadataRewrite(metadataPath, finalPath, originalContents, updatedContents));
            }

            return ExecuteRename(moves, rewrites);
        }
    }

    public ProjectContentFileOperationResult DeleteAssetGroup(string writableRoot, string assetInfoPath)
    {
        lock (mutationLock)
        {
            if (!TryResolveAssetGroup(writableRoot, assetInfoPath, out var root, out var group, out var error))
                return Failure(error);

            if (!TryCreateStagingDirectory(root, Path.GetDirectoryName(group.SourcePath)!, "asset", out var stagingDirectory, out error))
                return Failure(error);

            var moves = EnumerateAssetGroupPaths(group)
                .Select((sourcePath, index) => new FileMove(
                    sourcePath,
                    Path.Combine(stagingDirectory, index.ToString("D4", CultureInfo.InvariantCulture) + ".pending")))
                .ToArray();

            if (!TryPreflightFileMoves(root, moves, out error))
                return Failure(error);

            try
            {
                fileSystem.CreateDirectory(stagingDirectory);
            }
            catch (Exception exception)
            {
                return Failure($"Cannot prepare asset deletion: {exception.Message}");
            }

            var completedMoves = new List<FileMove>(moves.Length);
            try
            {
                foreach (var move in moves)
                {
                    completedMoves.Add(move);
                    fileSystem.MoveFile(move.SourcePath, move.DestinationPath);
                }

                fileSystem.DeleteDirectory(stagingDirectory, recursive: true);
                return Success();
            }
            catch (Exception exception)
            {
                var unrestored = RollbackFileMoves(completedMoves);
                TryRemoveStagingDirectory(stagingDirectory, unrestored);
                return Failure(
                    $"Cannot delete the asset group: {exception.Message}",
                    unrestored);
            }
        }
    }

    public ProjectContentFileOperationResult CreateFolder(
        string writableRoot,
        string parentDirectory,
        string newName)
    {
        lock (mutationLock)
        {
            if (!TryResolveWritableRoot(writableRoot, out var root, out var error))
                return Failure(error);

            if (!TryResolveDestinationDirectory(root, parentDirectory, out var parent, out error))
                return Failure(error);

            if (!TryNormalizePortableBasename(newName, out var folderName, out error))
                return Failure(error);

            var target = Path.Combine(parent, folderName);
            if (!ProjectContentPathPolicy.IsInsideRoot(root, target))
                return Failure("The new folder must stay inside the writable Content root.");
            if (PathExists(target))
                return Failure($"The destination already exists: {target}");

            try
            {
                fileSystem.CreateDirectory(target);
                return Success();
            }
            catch (Exception exception)
            {
                var unrestored = new List<string>();
                if (fileSystem.DirectoryExists(target))
                {
                    try
                    {
                        fileSystem.DeleteDirectory(target, recursive: false);
                    }
                    catch
                    {
                        unrestored.Add(target);
                    }
                }
                else if (fileSystem.FileExists(target))
                {
                    unrestored.Add(target);
                }

                return Failure($"Cannot create the folder: {exception.Message}", unrestored);
            }
        }
    }

    public ProjectContentFileOperationResult MoveFolder(
        string writableRoot,
        string sourceDirectory,
        string destinationDirectory)
    {
        lock (mutationLock)
        {
            if (!TryResolveMutableDirectory(writableRoot, sourceDirectory, out var root, out var source, out var error))
                return Failure(error);

            if (!TryResolveDestinationDirectory(root, destinationDirectory, out var destination, out error))
                return Failure(error);

            if (ProjectContentPathPolicy.IsSamePath(Path.GetDirectoryName(source)!, destination))
                return Failure("The folder is already in the destination directory.");

            if (ProjectContentPathPolicy.IsInsideRoot(source, destination))
                return Failure("A folder cannot be moved into itself or one of its descendants.");

            var target = Path.Combine(destination, Path.GetFileName(source));
            return MoveDirectory(root, source, target, "move the folder");
        }
    }

    public ProjectContentFileOperationResult RenameFolder(
        string writableRoot,
        string sourceDirectory,
        string newName)
    {
        lock (mutationLock)
        {
            if (!TryResolveMutableDirectory(writableRoot, sourceDirectory, out var root, out var source, out var error))
                return Failure(error);

            if (!TryNormalizePortableBasename(newName, out var targetName, out error))
                return Failure(error);

            var target = Path.Combine(Path.GetDirectoryName(source)!, targetName);
            if (ProjectContentPathPolicy.IsSamePath(source, target))
                return Failure("The folder already has that name.");

            return MoveDirectory(root, source, target, "rename the folder");
        }
    }

    public ProjectContentFileOperationResult DeleteFolder(string writableRoot, string sourceDirectory)
    {
        lock (mutationLock)
        {
            if (!TryResolveMutableDirectory(writableRoot, sourceDirectory, out var root, out var source, out var error))
                return Failure(error);

            if (!TryCreateStagingDirectory(root, Path.GetDirectoryName(source)!, "folder", out var stagingDirectory, out error))
                return Failure(error);

            try
            {
                fileSystem.MoveDirectory(source, stagingDirectory);
            }
            catch (Exception exception)
            {
                return Failure($"Cannot delete the folder: {exception.Message}");
            }

            try
            {
                fileSystem.DeleteDirectory(stagingDirectory, recursive: true);
                return Success();
            }
            catch (Exception exception)
            {
                // A recursive delete can remove descendants before reporting a failure.
                // Moving the remaining directory back cannot prove that its full contents
                // were restored, so report the source as unrestored conservatively.
                var unrestored = new List<string> { source };
                try
                {
                    if (fileSystem.DirectoryExists(stagingDirectory) && !PathExists(source))
                    {
                        fileSystem.MoveDirectory(stagingDirectory, source);
                    }
                }
                catch
                {
                }

                return Failure(
                    $"Cannot delete the folder: {exception.Message}",
                    unrestored);
            }
        }
    }

    bool TryPrepareDuplicateAssetGroup(
        string root,
        AssetGroup group,
        string primaryAssetInfoPath,
        out DuplicateAssetPlan plan,
        out string error)
    {
        plan = default!;
        var sourceFilename = Path.GetFileName(group.SourcePath);
        if (!TryAllocateDuplicatePaths(
            root,
            group,
            sourceFilename,
            out var targetSourcePath,
            out var targetMetadataPaths,
            out error))
        {
            return false;
        }

        var metadataDocuments = new List<DuplicateMetadataDocument>(group.AssetInfoPaths.Count);
        var fileIdReplacements = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var metadataPath in group.AssetInfoPaths.OrderBy(path => path, StringComparer.Ordinal))
        {
            if (!TryReadDuplicateMetadata(metadataPath, out var document, out error))
                return false;

            var newFileId = CreateFileId();
            if (!fileIdReplacements.TryAdd(document.OriginalFileId, newFileId))
            {
                error = $"Cannot duplicate asset metadata with a repeated fileId: {document.OriginalFileId}";
                return false;
            }

            metadataDocuments.Add(document with { NewFileId = newFileId });
        }

        var targetSourceFilename = Path.GetFileName(targetSourcePath);
        var metadataCreations = new List<MetadataCreation>(metadataDocuments.Count);
        foreach (var document in metadataDocuments)
        {
            RewriteScalarValues(document.Root, fileIdReplacements);

            var filenameKey = new YamlScalarNode("filename");
            if (!document.Root.Children.TryGetValue(filenameKey, out var filenameNode) ||
                filenameNode is not YamlScalarNode filenameScalar)
            {
                error = $"Cannot update asset metadata '{document.SourcePath}': Expected a filename field.";
                return false;
            }

            filenameScalar.Value = targetSourceFilename;
            string updatedContents;
            try
            {
                using var writer = new StringWriter(CultureInfo.InvariantCulture);
                document.Yaml.Save(writer, assignAnchors: false);
                updatedContents = writer.ToString();
            }
            catch (Exception exception)
            {
                error = $"Cannot serialize duplicated asset metadata '{document.SourcePath}': {exception.Message}";
                return false;
            }

            metadataCreations.Add(new MetadataCreation(
                document.SourcePath,
                targetMetadataPaths[document.SourcePath],
                document.NewFileId,
                updatedContents));
        }

        var primaryCreation = metadataCreations.SingleOrDefault(creation =>
            ProjectContentPathPolicy.IsSamePath(creation.SourcePath, primaryAssetInfoPath));
        if (primaryCreation is null)
        {
            error = "The duplicated asset group has no primary sidecar plan.";
            return false;
        }

        plan = new DuplicateAssetPlan(
            root,
            new FileCopy(group.SourcePath, targetSourcePath),
            metadataCreations,
            primaryCreation.DestinationPath,
            primaryCreation.FileId);
        error = string.Empty;
        return true;
    }

    bool TryAllocateDuplicatePaths(
        string root,
        AssetGroup group,
        string sourceFilename,
        out string targetSourcePath,
        out IReadOnlyDictionary<string, string> targetMetadataPaths,
        out string error)
    {
        targetSourcePath = string.Empty;
        targetMetadataPaths = new Dictionary<string, string>();
        var sourceDirectory = Path.GetDirectoryName(group.SourcePath)!;

        for (var copyNumber = 1; copyNumber <= 10_000; copyNumber++)
        {
            var targetSourceFilename = BuildDuplicateFilename(sourceFilename, copyNumber);
            var candidateSourcePath = Path.Combine(sourceDirectory, targetSourceFilename);
            var candidateMetadataPaths = new Dictionary<string, string>(ProjectContentPathPolicy.PathComparer);
            var destinations = new HashSet<string>(ProjectContentPathPolicy.PathComparer)
            {
                candidateSourcePath
            };
            var hasDuplicateDestination = false;

            foreach (var metadataPath in group.AssetInfoPaths)
            {
                var targetMetadataFilename = BuildDuplicateMetadataFilename(
                    Path.GetFileName(metadataPath),
                    sourceFilename,
                    targetSourceFilename,
                    copyNumber);
                var candidateMetadataPath = Path.Combine(sourceDirectory, targetMetadataFilename);
                if (!destinations.Add(candidateMetadataPath))
                {
                    hasDuplicateDestination = true;
                    break;
                }

                candidateMetadataPaths.Add(metadataPath, candidateMetadataPath);
            }

            if (hasDuplicateDestination)
                continue;
            if (destinations.Any(path => !ProjectContentPathPolicy.IsInsideRoot(root, path)))
            {
                error = "Every duplicated asset path must stay inside the writable Content root.";
                return false;
            }
            if (destinations.Any(PathExists))
                continue;

            targetSourcePath = candidateSourcePath;
            targetMetadataPaths = candidateMetadataPaths;
            error = string.Empty;
            return true;
        }

        error = "Cannot allocate a collision-free name for the duplicated asset group.";
        return false;
    }

    bool TryReadDuplicateMetadata(
        string metadataPath,
        out DuplicateMetadataDocument document,
        out string error)
    {
        document = default!;
        try
        {
            var yaml = new YamlStream();
            using var reader = new StringReader(fileSystem.ReadAllText(metadataPath));
            yaml.Load(reader);
            if (yaml.Documents.Count != 1 || yaml.Documents[0].RootNode is not YamlMappingNode root)
            {
                error = $"Asset metadata must contain one mapping document: {metadataPath}";
                return false;
            }

            var fileIdKey = new YamlScalarNode("fileId");
            if (!root.Children.TryGetValue(fileIdKey, out var fileIdNode) ||
                fileIdNode is not YamlScalarNode { Value: { } fileId } ||
                string.IsNullOrWhiteSpace(fileId))
            {
                error = $"Asset metadata must contain a scalar fileId: {metadataPath}";
                return false;
            }

            document = new DuplicateMetadataDocument(
                metadataPath,
                yaml,
                root,
                fileId,
                string.Empty);
            error = string.Empty;
            return true;
        }
        catch (Exception exception)
        {
            error = $"Cannot parse asset metadata '{metadataPath}': {exception.Message}";
            return false;
        }
    }

    ProjectContentFileOperationResult ExecuteDuplicate(DuplicateAssetPlan plan)
    {
        var destinations = plan.MetadataCreations
            .Select(creation => creation.DestinationPath)
            .Prepend(plan.SourceCopy.DestinationPath)
            .ToArray();
        if (destinations.Any(PathExists))
            return Failure("A destination for the duplicated asset group already exists.");
        if (!fileSystem.FileExists(plan.SourceCopy.SourcePath) ||
            !ProjectContentPathPolicy.IsInsideRoot(plan.Root, plan.SourceCopy.SourcePath) ||
            destinations.Any(path => !ProjectContentPathPolicy.IsInsideRoot(plan.Root, path)))
        {
            return Failure("Every source and destination must stay inside the writable Content root.");
        }

        var completedCreations = new List<string>(destinations.Length);
        string? uncertainCreation = null;
        try
        {
            uncertainCreation = plan.SourceCopy.DestinationPath;
            fileSystem.CopyFile(plan.SourceCopy.SourcePath, plan.SourceCopy.DestinationPath);
            completedCreations.Add(plan.SourceCopy.DestinationPath);
            uncertainCreation = null;

            foreach (var creation in plan.MetadataCreations)
            {
                uncertainCreation = creation.DestinationPath;
                fileSystem.WriteAllTextNew(creation.DestinationPath, creation.Contents);
                completedCreations.Add(creation.DestinationPath);
                uncertainCreation = null;
            }

            return Success(plan.PrimaryAssetInfoPath, plan.PrimaryFileId);
        }
        catch (Exception exception)
        {
            var unrestored = RollbackCreatedFiles(completedCreations);
            if (uncertainCreation is not null && PathExists(uncertainCreation))
                unrestored.Add(uncertainCreation);
            return Failure(
                $"Cannot duplicate the asset group: {exception.Message}",
                unrestored);
        }
    }

    List<string> RollbackCreatedFiles(IReadOnlyList<string> attemptedCreations)
    {
        var unrestored = new List<string>();
        foreach (var path in attemptedCreations
            .Distinct(ProjectContentPathPolicy.PathComparer)
            .Reverse())
        {
            try
            {
                if (fileSystem.FileExists(path))
                    fileSystem.DeleteFile(path);
                if (PathExists(path))
                    unrestored.Add(path);
            }
            catch
            {
                if (PathExists(path))
                    unrestored.Add(path);
            }
        }

        return unrestored
            .Distinct(ProjectContentPathPolicy.PathComparer)
            .OrderBy(path => path, StringComparer.Ordinal)
            .ToList();
    }

    ProjectContentFileOperationResult MoveDirectory(
        string root,
        string source,
        string target,
        string operationDescription)
    {
        string canonicalTarget;
        try
        {
            canonicalTarget = Path.GetFullPath(target);
        }
        catch (Exception exception)
        {
            return Failure($"The destination path is invalid: {exception.Message}");
        }

        if (!ProjectContentPathPolicy.IsInsideRoot(root, canonicalTarget))
            return Failure("The destination must stay inside the writable Content root.");
        if (ProjectContentPathPolicy.IsSamePath(source, canonicalTarget))
            return Failure("The requested folder operation would not change its path.");
        if (PathExists(canonicalTarget))
            return Failure($"The destination already exists: {canonicalTarget}");

        try
        {
            fileSystem.MoveDirectory(source, canonicalTarget);
            return Success();
        }
        catch (Exception exception)
        {
            return Failure($"Cannot {operationDescription}: {exception.Message}");
        }
    }

    ProjectContentFileOperationResult ExecuteFileMoves(
        IReadOnlyList<FileMove> moves,
        string operationDescription)
    {
        var completedMoves = new List<FileMove>(moves.Count);
        try
        {
            foreach (var move in moves)
            {
                completedMoves.Add(move);
                fileSystem.MoveFile(move.SourcePath, move.DestinationPath);
            }

            return Success();
        }
        catch (Exception exception)
        {
            var unrestored = RollbackFileMoves(completedMoves);
            return Failure(
                $"Cannot {operationDescription}: {exception.Message}",
                unrestored);
        }
    }

    ProjectContentFileOperationResult ExecuteRename(
        IReadOnlyList<FileMove> moves,
        IReadOnlyList<MetadataRewrite> rewrites)
    {
        var completedMoves = new List<FileMove>(moves.Count);
        var writesStarted = false;
        try
        {
            foreach (var move in moves)
            {
                completedMoves.Add(move);
                fileSystem.MoveFile(move.SourcePath, move.DestinationPath);
            }

            foreach (var rewrite in rewrites)
            {
                writesStarted = true;
                fileSystem.WriteAllText(rewrite.FinalPath, rewrite.UpdatedContents);
            }

            return Success();
        }
        catch (Exception exception)
        {
            var unrestored = new List<string>();
            if (writesStarted)
            {
                foreach (var rewrite in rewrites.Reverse())
                {
                    var currentPath = fileSystem.FileExists(rewrite.FinalPath)
                        ? rewrite.FinalPath
                        : rewrite.OriginalPath;
                    try
                    {
                        if (fileSystem.FileExists(currentPath))
                            fileSystem.WriteAllText(currentPath, rewrite.OriginalContents);
                        else
                            unrestored.Add(rewrite.OriginalPath);
                    }
                    catch
                    {
                        unrestored.Add(rewrite.OriginalPath);
                    }
                }
            }

            unrestored.AddRange(RollbackFileMoves(completedMoves));
            return Failure(
                $"Cannot rename the asset group: {exception.Message}",
                unrestored);
        }
    }

    List<string> RollbackFileMoves(IReadOnlyList<FileMove> completedMoves)
    {
        var unrestored = new List<string>();
        foreach (var move in completedMoves.Reverse())
        {
            try
            {
                if (fileSystem.FileExists(move.DestinationPath))
                {
                    if (PathExists(move.SourcePath))
                    {
                        unrestored.Add(move.SourcePath);
                    }
                    else
                    {
                        fileSystem.MoveFile(move.DestinationPath, move.SourcePath);
                    }
                }
                else if (!fileSystem.FileExists(move.SourcePath))
                {
                    unrestored.Add(move.SourcePath);
                }
            }
            catch
            {
                unrestored.Add(move.SourcePath);
            }
        }

        return unrestored
            .Distinct(ProjectContentPathPolicy.PathComparer)
            .OrderBy(path => path, StringComparer.Ordinal)
            .ToList();
    }

    bool TryResolveAssetGroup(
        string writableRoot,
        string assetInfoPath,
        out string root,
        out AssetGroup group,
        out string error)
    {
        root = string.Empty;
        group = default!;
        if (!TryResolveWritableRoot(writableRoot, out root, out error))
            return false;

        string metadataPath;
        try
        {
            metadataPath = ProjectContentPathPolicy.NormalizeRoot(assetInfoPath);
        }
        catch (Exception exception)
        {
            error = $"The asset metadata path is invalid: {exception.Message}";
            return false;
        }

        if (!fileSystem.FileExists(metadataPath))
        {
            error = $"Asset metadata does not exist: {metadataPath}";
            return false;
        }
        if (!string.Equals(Path.GetExtension(metadataPath), ".asset", StringComparison.OrdinalIgnoreCase))
        {
            error = "Asset operations require an .asset metadata path.";
            return false;
        }
        if (!ProjectContentPathPolicy.IsInsideRoot(root, metadataPath))
        {
            error = "The asset metadata is outside the writable Content root.";
            return false;
        }
        if (!TryReadDeclaredFilename(metadataPath, out var declaredFilename, out error))
            return false;
        if (!AssetSourcePathContract.TryResolve(metadataPath, declaredFilename, out var resolution, out error))
            return false;

        string sourcePath;
        try
        {
            sourcePath = ProjectContentPathPolicy.NormalizeRoot(resolution.SourcePath);
        }
        catch (Exception exception)
        {
            error = $"The asset source path is invalid: {exception.Message}";
            return false;
        }

        if (!fileSystem.FileExists(sourcePath) || !ProjectContentPathPolicy.IsInsideRoot(root, sourcePath))
        {
            error = "The asset source is outside the writable Content root.";
            return false;
        }

        var sourceDirectory = Path.GetDirectoryName(sourcePath)!;
        var relatedMetadata = new HashSet<string>(ProjectContentPathPolicy.PathComparer);
        IEnumerable<string> siblings;
        try
        {
            siblings = fileSystem.EnumerateFiles(sourceDirectory).ToArray();
        }
        catch (Exception exception)
        {
            error = $"Cannot enumerate asset metadata beside the source: {exception.Message}";
            return false;
        }

        foreach (var sibling in siblings
            .Where(path => string.Equals(Path.GetExtension(path), ".asset", StringComparison.OrdinalIgnoreCase))
            .OrderBy(path => path, StringComparer.Ordinal))
        {
            string siblingPath;
            try
            {
                siblingPath = ProjectContentPathPolicy.NormalizeRoot(sibling);
            }
            catch
            {
                continue;
            }

            if (!ProjectContentPathPolicy.IsInsideRoot(root, siblingPath) ||
                !TryReadDeclaredFilename(siblingPath, out var siblingFilename, out _) ||
                !AssetSourcePathContract.TryResolve(siblingPath, siblingFilename, out var siblingResolution, out _) ||
                !ProjectContentPathPolicy.IsSamePath(siblingResolution.SourcePath, sourcePath))
            {
                continue;
            }

            relatedMetadata.Add(siblingPath);
        }

        if (!relatedMetadata.Contains(metadataPath))
        {
            error = "The selected metadata could not be included in its source group.";
            return false;
        }

        group = new AssetGroup(
            sourcePath,
            relatedMetadata.OrderBy(path => path, StringComparer.Ordinal).ToArray());
        error = string.Empty;
        return true;
    }

    bool TryResolveWritableRoot(string writableRoot, out string root, out string error)
    {
        root = string.Empty;
        if (string.IsNullOrWhiteSpace(writableRoot))
        {
            error = "A writable Content root is required.";
            return false;
        }

        try
        {
            root = ProjectContentPathPolicy.NormalizeRoot(writableRoot);
        }
        catch (Exception exception)
        {
            error = $"The writable Content root is invalid: {exception.Message}";
            return false;
        }

        if (!fileSystem.DirectoryExists(root))
        {
            error = $"The writable Content root does not exist: {root}";
            return false;
        }

        error = string.Empty;
        return true;
    }

    bool TryResolveMutableDirectory(
        string writableRoot,
        string sourceDirectory,
        out string root,
        out string source,
        out string error)
    {
        source = string.Empty;
        if (!TryResolveWritableRoot(writableRoot, out root, out error))
            return false;

        try
        {
            source = ProjectContentPathPolicy.NormalizeRoot(sourceDirectory);
        }
        catch (Exception exception)
        {
            error = $"The folder path is invalid: {exception.Message}";
            return false;
        }

        if (!fileSystem.DirectoryExists(source))
        {
            error = $"The folder does not exist: {source}";
            return false;
        }
        if (!ProjectContentPathPolicy.IsInsideRoot(root, source))
        {
            error = "The folder is outside the writable Content root.";
            return false;
        }
        if (ProjectContentPathPolicy.IsSamePath(root, source))
        {
            error = "The Content mount root cannot be moved, renamed, or deleted.";
            return false;
        }

        error = string.Empty;
        return true;
    }

    bool TryResolveDestinationDirectory(
        string root,
        string destinationDirectory,
        out string destination,
        out string error)
    {
        destination = string.Empty;
        try
        {
            destination = ProjectContentPathPolicy.NormalizeRoot(destinationDirectory);
        }
        catch (Exception exception)
        {
            error = $"The destination path is invalid: {exception.Message}";
            return false;
        }

        if (!fileSystem.DirectoryExists(destination))
        {
            error = $"The destination folder does not exist: {destination}";
            return false;
        }
        if (!ProjectContentPathPolicy.IsInsideRoot(root, destination))
        {
            error = "The destination is outside the writable Content root.";
            return false;
        }

        error = string.Empty;
        return true;
    }

    bool TryPreflightFileMoves(string root, IReadOnlyList<FileMove> moves, out string error)
    {
        var sources = new HashSet<string>(ProjectContentPathPolicy.PathComparer);
        var destinations = new HashSet<string>(ProjectContentPathPolicy.PathComparer);
        foreach (var move in moves)
        {
            if (!sources.Add(move.SourcePath) || !destinations.Add(move.DestinationPath))
            {
                error = "The asset operation contains duplicate file paths.";
                return false;
            }
            if (!fileSystem.FileExists(move.SourcePath))
            {
                error = $"A source file no longer exists: {move.SourcePath}";
                return false;
            }
            if (!ProjectContentPathPolicy.IsInsideRoot(root, move.SourcePath) ||
                !ProjectContentPathPolicy.IsInsideRoot(root, move.DestinationPath))
            {
                error = "Every source and destination must stay inside the writable Content root.";
                return false;
            }
            if (ProjectContentPathPolicy.IsSamePath(move.SourcePath, move.DestinationPath))
            {
                error = "The requested asset operation would not change every planned path.";
                return false;
            }
            if (PathExists(move.DestinationPath))
            {
                error = $"The destination already exists: {move.DestinationPath}";
                return false;
            }
        }

        error = string.Empty;
        return true;
    }

    bool TryReadDeclaredFilename(string metadataPath, out string filename, out string error)
    {
        filename = string.Empty;
        try
        {
            var yaml = new YamlStream();
            using var reader = new StringReader(fileSystem.ReadAllText(metadataPath));
            yaml.Load(reader);
            if (yaml.Documents.Count != 1 ||
                yaml.Documents[0].RootNode is not YamlMappingNode root ||
                !root.Children.TryGetValue(new YamlScalarNode("filename"), out var filenameNode) ||
                filenameNode is not YamlScalarNode { Value: { } value } ||
                string.IsNullOrWhiteSpace(value))
            {
                error = $"Asset metadata must contain one mapping document with a scalar filename: {metadataPath}";
                return false;
            }

            filename = value;
            error = string.Empty;
            return true;
        }
        catch (Exception exception)
        {
            error = $"Cannot parse asset metadata '{metadataPath}': {exception.Message}";
            return false;
        }
    }

    static bool TryRewriteDeclaredFilename(
        string originalContents,
        string filename,
        out string updatedContents,
        out string error)
    {
        updatedContents = string.Empty;
        try
        {
            var yaml = new YamlStream();
            using (var reader = new StringReader(originalContents))
            {
                yaml.Load(reader);
            }

            if (yaml.Documents.Count != 1 || yaml.Documents[0].RootNode is not YamlMappingNode root)
            {
                error = "Expected one YAML mapping document.";
                return false;
            }

            var key = new YamlScalarNode("filename");
            if (!root.Children.ContainsKey(key))
            {
                error = "Expected a filename field.";
                return false;
            }

            root.Children[key] = new YamlScalarNode(filename);
            using var writer = new StringWriter(CultureInfo.InvariantCulture);
            yaml.Save(writer, assignAnchors: false);
            updatedContents = writer.ToString();
            error = string.Empty;
            return true;
        }
        catch (Exception exception)
        {
            error = exception.Message;
            return false;
        }
    }

    static string BuildDuplicateFilename(string sourceFilename, int copyNumber)
    {
        var extension = Path.GetExtension(sourceFilename);
        var stem = sourceFilename[..^extension.Length];
        if (string.IsNullOrEmpty(stem))
        {
            stem = sourceFilename;
            extension = string.Empty;
        }

        return stem + BuildDuplicateSuffix(copyNumber) + extension;
    }

    static string BuildDuplicateMetadataFilename(
        string metadataFilename,
        string sourceFilename,
        string targetSourceFilename,
        int copyNumber)
    {
        if (metadataFilename.StartsWith(sourceFilename, ProjectContentPathPolicy.PathComparison))
            return targetSourceFilename + metadataFilename[sourceFilename.Length..];

        var extension = Path.GetExtension(metadataFilename);
        var stem = metadataFilename[..^extension.Length];
        return stem + BuildDuplicateSuffix(copyNumber) + extension;
    }

    static string BuildDuplicateSuffix(int copyNumber)
        => copyNumber == 1 ? " Copy" : $" Copy {copyNumber}";

    static string CreateFileId()
        => $"{{{Guid.NewGuid().ToString().ToUpperInvariant()}}}";

    static void RewriteScalarValues(
        YamlNode node,
        IReadOnlyDictionary<string, string> replacements)
    {
        switch (node)
        {
            case YamlScalarNode { Value: { } value } scalar
                when replacements.TryGetValue(value, out var replacement):
                scalar.Value = replacement;
                break;

            case YamlMappingNode mapping:
                foreach (var child in mapping.Children.Values)
                    RewriteScalarValues(child, replacements);
                break;

            case YamlSequenceNode sequence:
                foreach (var child in sequence.Children)
                    RewriteScalarValues(child, replacements);
                break;
        }
    }

    static bool TryNormalizePortableBasename(
        string? value,
        out string basename,
        out string error)
    {
        basename = value?.Trim() ?? string.Empty;
        if (string.IsNullOrWhiteSpace(basename) ||
            basename is "." or ".." ||
            Path.IsPathRooted(basename) ||
            basename.Contains('/') ||
            basename.Contains('\\') ||
            PortableInvalidFilenameCharacters.Any(basename.Contains) ||
            basename.Any(character => char.IsControl(character)) ||
            basename.EndsWith('.') ||
            basename.EndsWith(' ') ||
            !string.Equals(Path.GetFileName(basename), basename, StringComparison.Ordinal))
        {
            error = "The name must be a portable filename without path separators or traversal.";
            return false;
        }

        var stem = basename.Split('.', 2)[0];
        if (ReservedPortableNames.Contains(stem))
        {
            error = "The name is reserved on supported platforms.";
            return false;
        }

        error = string.Empty;
        return true;
    }

    bool TryCreateStagingDirectory(
        string root,
        string parentDirectory,
        string operation,
        out string stagingDirectory,
        out string error)
    {
        for (var attempt = 0; attempt < 8; attempt++)
        {
            stagingDirectory = Path.Combine(
                parentDirectory,
                $".sailor-{operation}-{Guid.NewGuid():N}.pending");
            if (!PathExists(stagingDirectory) && ProjectContentPathPolicy.IsInsideRoot(root, stagingDirectory))
            {
                error = string.Empty;
                return true;
            }
        }

        stagingDirectory = string.Empty;
        error = "Cannot allocate a temporary path for the content operation.";
        return false;
    }

    void TryRemoveStagingDirectory(string stagingDirectory, List<string> unrestored)
    {
        if (!fileSystem.DirectoryExists(stagingDirectory))
            return;

        try
        {
            fileSystem.DeleteDirectory(stagingDirectory, recursive: true);
        }
        catch
        {
            unrestored.Add(stagingDirectory);
        }
    }

    bool PathExists(string path) => fileSystem.FileExists(path) || fileSystem.DirectoryExists(path);

    static IEnumerable<string> EnumerateAssetGroupPaths(AssetGroup group)
    {
        yield return group.SourcePath;
        foreach (var path in group.AssetInfoPaths)
            yield return path;
    }

    static ProjectContentFileOperationResult Success(
        string? createdAssetInfoPath = null,
        string? createdFileId = null)
        => new(
            true,
            null,
            true,
            Array.Empty<string>(),
            createdAssetInfoPath,
            createdFileId);

    static ProjectContentFileOperationResult Failure(
        string error,
        IEnumerable<string>? unrestoredPaths = null)
    {
        var unrestored = (unrestoredPaths ?? Array.Empty<string>())
            .Distinct(ProjectContentPathPolicy.PathComparer)
            .OrderBy(path => path, StringComparer.Ordinal)
            .ToArray();
        return new(false, error, unrestored.Length == 0, unrestored);
    }

    sealed record AssetGroup(string SourcePath, IReadOnlyList<string> AssetInfoPaths);
    sealed record DuplicateAssetPlan(
        string Root,
        FileCopy SourceCopy,
        IReadOnlyList<MetadataCreation> MetadataCreations,
        string PrimaryAssetInfoPath,
        string PrimaryFileId);
    sealed record DuplicateMetadataDocument(
        string SourcePath,
        YamlStream Yaml,
        YamlMappingNode Root,
        string OriginalFileId,
        string NewFileId);
    sealed record FileCopy(string SourcePath, string DestinationPath);
    sealed record FileMove(string SourcePath, string DestinationPath);
    sealed record MetadataCreation(
        string SourcePath,
        string DestinationPath,
        string FileId,
        string Contents);
    sealed record MetadataRewrite(
        string OriginalPath,
        string FinalPath,
        string OriginalContents,
        string UpdatedContents);
}
