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
    public sealed record PrefabAssetWriteResult(
        PrefabFile Prefab,
        ProjectContentAssetWriteTransaction Transaction);

    public class AssetsService : IDisposable
    {
        const int ActiveProjectRootId = 1;
        const int EngineProjectRootId = 2;

        readonly string _engineContentDirectory;
        readonly EngineService _engineService;
        readonly ProjectContentFileOperations _fileOperations = new();
        readonly object _watcherLock = new();
        readonly List<FileSystemWatcher> _contentWatchers = [];
        readonly SemaphoreSlim _assetSaveLock = new(1, 1);
        readonly SemaphoreSlim _folderLoadLock = new(1, 1);
        readonly AssetCacheIndexStore _assetCacheIndexStore = new();
        Task _assetCacheIndexLoadTask = Task.CompletedTask;
        EngineLaunchContext? _activeLaunchContext;
        CancellationTokenSource? _pendingFilesystemReload;
        CancellationTokenSource? _assetCacheIndexLoadCancellation;
        ProjectContentFolderIdAllocator _folderIdAllocator = new();
        HashSet<string> _visitedDirectories = new(ProjectContentPathPolicy.PathComparer);
        int _nextAssetId = 1;
        int _assetOverrideCount;
        long _workspaceEpoch;
        long _contentGeneration;

        public event Action? Changed;

        public ProjectRoot Root { get; private set; } = new() { Name = string.Empty };
        public List<AssetFolder> Folders { get; private set; } = [];
        public Dictionary<FileId, AssetFile> Assets { get; private set; } = [];
        public List<AssetFile> Files { get; private set; } = new();
        public string CurrentProjectRootPath { get; private set; } = string.Empty;
        public EditorProjectMode CurrentProjectMode { get; private set; } = EditorProjectMode.Engine;
        public long WorkspaceEpoch => Interlocked.Read(ref _workspaceEpoch);

        public AssetsService()
        {
            var engineService = MauiProgram.GetService<EngineService>();
            _engineService = engineService;
            engineService.OnAssetReloadCompleted += HandleAssetReloadCompleted;
            var engineContentDirectory = Path.GetFullPath(engineService.EngineContentDirectory);
            Directory.CreateDirectory(engineContentDirectory);
            _engineContentDirectory = ProjectContentPathPolicy.NormalizeRoot(engineContentDirectory);
            AddProjectRoot(engineService.GetLaunchContext());
        }

        async void HandleAssetReloadCompleted(AssetReloadCompletion completion)
        {
            if (!completion.Succeeded || _activeLaunchContext is null)
            {
                return;
            }

            try
            {
                var selectionService = MauiProgram.GetService<SelectionService>();
                var selectedAssetId = (selectionService.SelectedItem as AssetFile)?.FileId;
                await RefreshAsync();
                await MainThread.InvokeOnMainThreadAsync(() =>
                {
                    if (selectedAssetId is null || selectedAssetId.IsEmpty())
                    {
                        return;
                    }

                    if (Assets.TryGetValue(selectedAssetId, out var refreshedAsset))
                    {
                        selectionService.SelectObject(refreshedAsset, force: true);
                        return;
                    }

                    selectionService.ClearSelection();
                });
            }
            catch (Exception exception)
            {
                Console.WriteLine(
                    $"[AssetsService] Failed to refresh after asset reload generation {completion.Generation}: {exception.Message}");
            }
        }

        public Task<bool> SaveAssetAsync(
            AssetFile assetFile,
            CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(assetFile);
            return SaveExistingAssetAsync(
                assetFile.FileId,
                assetFile.Save,
                cancellationToken);
        }

        public async Task<bool> SaveExistingAssetAsync(
            FileId fileId,
            Func<Task> saveAsync,
            CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(fileId);
            ArgumentNullException.ThrowIfNull(saveAsync);
            if (fileId.IsEmpty())
            {
                return false;
            }

            await _assetSaveLock.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            try
            {
                (FileSystemWatcher Watcher, bool Enabled)[] watcherStates;
                lock (_watcherLock)
                {
                    watcherStates = _contentWatchers
                        .Select(watcher =>
                            (watcher, watcher.EnableRaisingEvents))
                        .ToArray();
                    foreach (var watcherState in watcherStates)
                    {
                        watcherState.Watcher.EnableRaisingEvents = false;
                    }
                }

                try
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    await saveAsync().ConfigureAwait(false);
                }
                finally
                {
                    lock (_watcherLock)
                    {
                        foreach (var watcherState in watcherStates)
                        {
                            if (_contentWatchers.Contains(watcherState.Watcher))
                            {
                                watcherState.Watcher.EnableRaisingEvents =
                                    watcherState.Enabled;
                            }
                        }
                    }
                }

                return await _engineService.UpdateAssetAsync(
                        fileId,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                _assetSaveLock.Release();
            }
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

        public async Task<AssetFile?> CreateAnimationAssetAsync(
            AssetFolder? targetFolder,
            bool createSet,
            CancellationToken cancellationToken = default)
        {
            if (!TryGetWritableDestinationDirectory(
                    targetFolder,
                    out var destinationDirectory))
            {
                return null;
            }

            var extension = createSet ? ".animset" : ".animcontroller";
            var baseName = createSet ? "New Animation Set" : "New Animation Controller";
            var filename = GetUniqueAssetName(
                destinationDirectory,
                baseName,
                extension);
            var sourcePath = Path.Combine(destinationDirectory, filename);
            var fileId = new FileId(
                Guid.NewGuid().ToString().ToUpperInvariant());
            var sourceContents = createSet
                ? CreateAnimationSetSource()
                : CreateAnimationControllerSource();
            var metadataContents = SerializeAnimationAssetInfo(
                fileId,
                filename,
                createSet);

            await _assetSaveLock.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
            try
            {
                ProjectContentAssetWriteTransaction transaction;
                (FileSystemWatcher Watcher, bool Enabled)[] watcherStates;
                lock (_watcherLock)
                {
                    watcherStates = _contentWatchers
                        .Select(watcher =>
                            (watcher, watcher.EnableRaisingEvents))
                        .ToArray();
                    foreach (var watcherState in watcherStates)
                    {
                        watcherState.Watcher.EnableRaisingEvents = false;
                    }
                }

                try
                {
                    transaction = _fileOperations.BeginWriteAssetPair(
                        CurrentProjectRootPath,
                        sourcePath,
                        sourceContents,
                        metadataContents,
                        fileId.Value,
                        overwrite: false);
                }
                finally
                {
                    lock (_watcherLock)
                    {
                        foreach (var watcherState in watcherStates)
                        {
                            if (_contentWatchers.Contains(watcherState.Watcher))
                            {
                                watcherState.Watcher.EnableRaisingEvents =
                                    watcherState.Enabled;
                            }
                        }
                    }
                }

                if (!transaction.Result.Succeeded)
                {
                    return null;
                }

                AssetFile? created;
                try
                {
                    created = await MainThread.InvokeOnMainThreadAsync(() =>
                        PublishCreatedAnimationAsset(
                            sourcePath,
                            targetFolder,
                            fileId));
                }
                catch
                {
                    transaction.Rollback();
                    throw;
                }

                if (created is null)
                {
                    transaction.Rollback();
                    return null;
                }

                var commit = transaction.Commit();
                if (!commit.Succeeded)
                {
                    await MainThread.InvokeOnMainThreadAsync(Refresh);
                    return null;
                }

                QueueAnimationAssetReload();
                return created;
            }
            finally
            {
                _assetSaveLock.Release();
            }
        }

        AssetFile? FindAssetBySourcePath(string sourcePath) =>
            Files.FirstOrDefault(asset =>
                asset.Asset is not null &&
                ProjectContentPathPolicy.IsSamePath(
                    asset.Asset.FullName,
                    sourcePath));

        AssetFile? PublishCreatedAnimationAsset(
            string sourcePath,
            AssetFolder? targetFolder,
            FileId expectedFileId)
        {
            var created = FindAssetBySourcePath(sourcePath);
            if (created is not null)
            {
                return created;
            }

            var destinationDirectory = Path.GetDirectoryName(sourcePath);
            var liveFolder = targetFolder;
            if (liveFolder is null ||
                string.IsNullOrWhiteSpace(destinationDirectory) ||
                !ProjectContentPathPolicy.IsSamePath(
                    liveFolder.FullPath,
                    destinationDirectory))
            {
                liveFolder = Folders.FirstOrDefault(folder =>
                    !string.IsNullOrWhiteSpace(destinationDirectory) &&
                    ProjectContentPathPolicy.IsSamePath(
                        folder.FullPath,
                        destinationDirectory));
            }

            var projectRootId = liveFolder?.ProjectRootId ??
                (CurrentProjectMode == EditorProjectMode.Engine
                    ? EngineProjectRootId
                    : ActiveProjectRootId);
            var asset = ReadAssetFile(
                new FileInfo(sourcePath + ".asset"),
                liveFolder?.Id ?? -1,
                projectRootId,
                isReadOnly: false);
            if (asset.FileId is null || !asset.FileId.Equals(expectedFileId))
            {
                return null;
            }

            Files.Add(asset);
            Assets[expectedFileId] = asset;
            if (liveFolder is not null)
            {
                liveFolder.HasChildren = true;
            }
            Changed?.Invoke();
            return asset;
        }

        void QueueAnimationAssetReload()
            => _ = ReloadCreatedAnimationAssetAsync();

        async Task ReloadCreatedAnimationAssetAsync()
        {
            try
            {
                if (!await _engineService.RequestAssetReloadAsync()
                        .ConfigureAwait(false))
                {
                    Console.WriteLine(
                        "[AssetsService] Engine did not register a newly created animation asset.");
                }
            }
            catch (Exception exception)
            {
                Console.WriteLine(
                    $"[AssetsService] Failed to register a newly created animation asset: {exception.Message}");
            }
        }

        static string CreateAnimationControllerSource()
        {
            var stateId = BitConverter.ToUInt64(
                System.Security.Cryptography.RandomNumberGenerator.GetBytes(
                    sizeof(ulong)));
            if (stateId == 0)
            {
                stateId = 1;
            }
            var root = new YamlMappingNode
            {
                { "version", "1" },
                { "defaultState", stateId.ToString(System.Globalization.CultureInfo.InvariantCulture) },
                { "parameters", new YamlSequenceNode() },
                {
                    "states",
                    new YamlSequenceNode
                    {
                        new YamlMappingNode
                        {
                            { "id", stateId.ToString(System.Globalization.CultureInfo.InvariantCulture) },
                            { "name", "State" },
                            { "clip", "Animation" },
                            { "speed", "1" },
                            { "loop", "true" },
                            {
                                "editor",
                                new YamlMappingNode
                                {
                                    { "x", "32" },
                                    { "y", "32" }
                                }
                            }
                        }
                    }
                },
                { "transitions", new YamlSequenceNode() }
            };
            return SaveYaml(root);
        }

        static string CreateAnimationSetSource() => SaveYaml(
            new YamlMappingNode
            {
                { "version", "1" },
                { "clips", new YamlSequenceNode() }
            });

        static string SerializeAnimationAssetInfo(
            FileId fileId,
            string filename,
            bool createSet) => SaveYaml(
            new YamlMappingNode
            {
                {
                    "assetInfoType",
                    createSet
                        ? "Sailor::AnimationSetAssetInfo"
                        : "Sailor::AnimationControllerAssetInfo"
                },
                { "fileId", fileId.Value },
                { "filename", filename }
            });

        static string SaveYaml(YamlMappingNode root)
        {
            var yaml = new YamlStream(new YamlDocument(root));
            using var writer = new StringWriter(
                System.Globalization.CultureInfo.InvariantCulture);
            yaml.Save(writer, false);
            return writer.ToString();
        }

        public PrefabFile? CreatePrefabAsset(
            AssetFolder? targetFolder,
            GameObject root,
            bool overwrite = false,
            PrefabFile? existingPrefab = null)
        {
            var creation = BeginCreatePrefabAsset(
                targetFolder,
                root,
                overwrite,
                existingPrefab);
            if (creation is null)
                return null;

            var commit = CompletePrefabAssetWrite(
                creation.Transaction,
                commit: true);
            if (!commit.Succeeded)
            {
                CompletePrefabAssetWrite(
                    creation.Transaction,
                    commit: false);
            }
            return commit.Succeeded
                ? creation.Prefab
                : null;
        }

        public PrefabAssetWriteResult? BeginCreatePrefabAsset(
            AssetFolder? targetFolder,
            GameObject root,
            bool overwrite = false,
            PrefabFile? existingPrefab = null)
        {
            if ((targetFolder?.IsReadOnly ?? false) || (existingPrefab?.IsReadOnly ?? false))
                return null;

            var worldService = MauiProgram.GetService<WorldService>();
            var prefab = worldService.CreatePrefabFromSubHierarchy(root, out var externalRefs);
            var folderPath = existingPrefab?.Asset?.DirectoryName ?? GetFolderPath(targetFolder);
            if (!ProjectContentPathPolicy.IsInsideRoot(CurrentProjectRootPath, folderPath))
                return null;

            if (!Directory.Exists(folderPath))
                return null;

            var prefabName = existingPrefab?.Asset?.Name ?? GetUniqueAssetName(folderPath, root.Name, ".prefab");
            var prefabPath = existingPrefab?.Asset?.FullName ?? Path.Combine(folderPath, prefabName);
            var fileId = existingPrefab?.FileId ?? new FileId($"{{{Guid.NewGuid().ToString().ToUpperInvariant()}}}");
            var sourceContents =
                SailorEditor.Commands.EditorYaml.SerializePrefab(prefab);
            var metadataContents = SerializePrefabAssetInfo(
                fileId,
                Path.GetFileName(prefabPath),
                externalRefs);

            ProjectContentAssetWriteTransaction transaction;
            lock (_watcherLock)
            {
                var watcherStates = _contentWatchers
                    .Select(watcher => watcher.EnableRaisingEvents)
                    .ToArray();
                try
                {
                    foreach (var watcher in _contentWatchers)
                        watcher.EnableRaisingEvents = false;
                    transaction = _fileOperations.BeginWriteAssetPair(
                        CurrentProjectRootPath,
                        prefabPath,
                        sourceContents,
                        metadataContents,
                        fileId.Value,
                        overwrite);
                }
                finally
                {
                    for (var index = 0;
                        index < _contentWatchers.Count;
                        index++)
                    {
                        _contentWatchers[index].EnableRaisingEvents =
                            watcherStates[index];
                    }
                }
            }

            if (!transaction.Result.Succeeded)
                return null;

            try
            {
                Refresh();
            }
            catch
            {
                CompletePrefabAssetWrite(transaction, commit: false);
                return null;
            }

            var created = Assets.TryGetValue(fileId, out var asset) && asset is PrefabFile prefabFile
                ? prefabFile
                : null;
            if (created is null)
            {
                CompletePrefabAssetWrite(transaction, commit: false);
                return null;
            }

            return new PrefabAssetWriteResult(created, transaction);
        }

        public ProjectContentFileOperationResult CompletePrefabAssetWrite(
            ProjectContentAssetWriteTransaction transaction,
            bool commit)
        {
            ProjectContentFileOperationResult result;
            lock (_watcherLock)
            {
                var watcherStates = _contentWatchers
                    .Select(watcher => watcher.EnableRaisingEvents)
                    .ToArray();
                try
                {
                    foreach (var watcher in _contentWatchers)
                        watcher.EnableRaisingEvents = false;
                    result = commit
                        ? transaction.Commit()
                        : transaction.Rollback();
                }
                finally
                {
                    for (var index = 0;
                        index < _contentWatchers.Count;
                        index++)
                    {
                        _contentWatchers[index].EnableRaisingEvents =
                            watcherStates[index];
                    }
                }
            }

            if (!commit || !result.Succeeded)
                Refresh();
            return result;
        }

        public ProjectContentFileOperationResult RenameAsset(AssetFile assetFile, string newName)
        {
            if (!CanRenameAsset(assetFile) || string.IsNullOrWhiteSpace(newName))
            {
                return FileOperationFailure("The asset cannot be renamed in the active Content root.");
            }

            return ExecuteContentMutation(() =>
                _fileOperations.RenameAssetGroup(
                    CurrentProjectRootPath,
                    assetFile.AssetInfo.FullName,
                    newName));
        }

        public ProjectContentFileOperationResult DeleteAsset(AssetFile assetFile)
        {
            if (!CanModifyAsset(assetFile))
                return FileOperationFailure("The asset cannot be deleted from the active Content root.");

            return ExecuteContentMutation(() =>
                _fileOperations.DeleteAssetGroup(
                    CurrentProjectRootPath,
                    assetFile.AssetInfo.FullName));
        }

        public ProjectContentFileOperationResult DuplicateAsset(AssetFile assetFile)
        {
            if (!CanDuplicateAsset(assetFile))
                return FileOperationFailure("The asset cannot be duplicated in the active Content root.");

            return ExecuteContentMutation(() =>
                _fileOperations.DuplicateAssetGroup(
                    CurrentProjectRootPath,
                    assetFile.AssetInfo.FullName));
        }

        public ProjectContentFileOperationResult MoveAsset(AssetFile assetFile, AssetFolder? destinationFolder)
        {
            if (!CanMoveAsset(assetFile, destinationFolder) || !TryGetWritableDestinationDirectory(destinationFolder, out var destinationDirectory))
                return FileOperationFailure("The asset cannot be moved to the selected Content folder.");

            return ExecuteContentMutation(() =>
                _fileOperations.MoveAssetGroup(
                    CurrentProjectRootPath,
                    assetFile.AssetInfo.FullName,
                    destinationDirectory));
        }

        public ProjectContentFileOperationResult MoveFolder(AssetFolder folder, AssetFolder? destinationFolder)
        {
            if (!CanMoveFolder(folder, destinationFolder) || !TryGetWritableDestinationDirectory(destinationFolder, out var destinationDirectory))
                return FileOperationFailure("The folder cannot be moved to the selected Content folder.");

            var sourceDirectory = folder.FullPath;
            var targetDirectory = Path.Combine(destinationDirectory, Path.GetFileName(sourceDirectory));
            var folderRebindPlan = CreateFolderRebindPlan(
                sourceDirectory,
                targetDirectory,
                ProjectContentFolderMutationKind.MoveOrRename);

            return ExecuteContentMutation(() =>
                _fileOperations.MoveFolder(
                    CurrentProjectRootPath,
                    sourceDirectory,
                    destinationDirectory),
                folderRebindPlan);
        }

        public ProjectContentFileOperationResult CreateFolder(AssetFolder? parentFolder, string folderName)
        {
            if (!CanCreateFolder(parentFolder) ||
                string.IsNullOrWhiteSpace(folderName) ||
                !TryGetWritableDestinationDirectory(parentFolder, out var parentDirectory))
            {
                return FileOperationFailure("A folder cannot be created in the selected Content directory.");
            }

            var createdFolderPath = Path.Combine(parentDirectory, folderName.Trim());
            return ExecuteContentMutation(
                () => _fileOperations.CreateFolder(
                    CurrentProjectRootPath,
                    parentDirectory,
                    folderName),
                successfulFolderSelectionPath: createdFolderPath);
        }

        public ProjectContentFileOperationResult RenameFolder(AssetFolder folder, string newName)
        {
            if (!CanModifyFolder(folder) || string.IsNullOrWhiteSpace(newName))
                return FileOperationFailure("The folder cannot be renamed in the active Content root.");

            var sourceDirectory = folder.FullPath;
            var folderRebindPlan = CreateRenameFolderRebindPlan(sourceDirectory, newName);

            return ExecuteContentMutation(() =>
                _fileOperations.RenameFolder(
                    CurrentProjectRootPath,
                    sourceDirectory,
                    newName),
                folderRebindPlan);
        }

        public ProjectContentFileOperationResult DeleteFolder(AssetFolder folder)
        {
            if (!CanModifyFolder(folder))
                return FileOperationFailure("The folder cannot be deleted from the active Content root.");

            var folderRebindPlan = CreateFolderRebindPlan(
                folder.FullPath,
                null,
                ProjectContentFolderMutationKind.Delete);

            return ExecuteContentMutation(() =>
                _fileOperations.DeleteFolder(
                    CurrentProjectRootPath,
                    folder.FullPath),
                folderRebindPlan);
        }

        ProjectContentFileOperationResult ExecuteContentMutation(
            Func<ProjectContentFileOperationResult> mutation,
            ProjectContentFolderRebindPlan? folderRebindPlan = null,
            string? successfulFolderSelectionPath = null)
        {
            var selectionService = MauiProgram.GetService<SelectionService>();
            var selectedAssetId = (selectionService.SelectedItem as AssetFile)?.FileId;
            ProjectContentFileOperationResult result;

            lock (_watcherLock)
            {
                var watcherStates = _contentWatchers
                    .Select(watcher => watcher.EnableRaisingEvents)
                    .ToArray();

                try
                {
                    foreach (var watcher in _contentWatchers)
                    {
                        watcher.EnableRaisingEvents = false;
                    }

                    result = mutation();
                }
                finally
                {
                    for (var index = 0; index < _contentWatchers.Count; index++)
                    {
                        _contentWatchers[index].EnableRaisingEvents = watcherStates[index];
                    }
                }
            }

            if (result.Succeeded || !result.RollbackSucceeded)
            {
                Refresh();
                if (result.Succeeded && !string.IsNullOrWhiteSpace(result.CreatedFileId))
                {
                    var createdFileId = new FileId(result.CreatedFileId);
                    if (Assets.TryGetValue(createdFileId, out var createdAsset))
                        selectionService.SelectObject(createdAsset, force: true);
                    else
                        selectionService.ClearSelection();
                }
                else if (result.Succeeded && !string.IsNullOrWhiteSpace(successfulFolderSelectionPath))
                {
                    if (selectionService.SelectedItem is AssetFile)
                        selectionService.ClearSelection();

                    MauiProgram.GetService<ProjectContentStore>().RestoreFolderSelectionByPath(
                        successfulFolderSelectionPath);
                }
                else if (folderRebindPlan is { HasSelection: true } plan)
                {
                    if (selectionService.SelectedItem is AssetFile)
                        selectionService.ClearSelection();

                    MauiProgram.GetService<ProjectContentStore>().RestoreFolderSelectionByPath(
                        result.Succeeded ? plan.SuccessPath : plan.OriginalPath);
                }
                else
                {
                    RestoreSelectedAsset(selectionService, selectedAssetId);
                }
            }

            return result;
        }

        static ProjectContentFolderRebindPlan CreateFolderRebindPlan(
            string sourceFolderPath,
            string? targetFolderPath,
            ProjectContentFolderMutationKind mutationKind)
        {
            try
            {
                var selectedFolderPath = MauiProgram.GetService<ProjectContentStore>().GetSelectedFolderPath();
                return ProjectContentFolderRebindPolicy.CreatePlan(
                    selectedFolderPath,
                    sourceFolderPath,
                    targetFolderPath,
                    mutationKind);
            }
            catch
            {
                return default;
            }
        }

        static ProjectContentFolderRebindPlan CreateRenameFolderRebindPlan(
            string sourceFolderPath,
            string newName)
        {
            try
            {
                var targetFolderPath = Path.Combine(Path.GetDirectoryName(sourceFolderPath)!, newName.Trim());
                return CreateFolderRebindPlan(
                    sourceFolderPath,
                    targetFolderPath,
                    ProjectContentFolderMutationKind.MoveOrRename);
            }
            catch
            {
                return default;
            }
        }

        void RestoreSelectedAsset(SelectionService selectionService, FileId? selectedAssetId)
        {
            if (selectedAssetId is null || selectedAssetId.IsEmpty())
                return;

            if (Assets.TryGetValue(selectedAssetId, out var refreshedAsset))
            {
                selectionService.SelectObject(refreshedAsset, force: true);
            }
            else
            {
                selectionService.ClearSelection();
            }
        }

        public void Refresh()
        {
            if (_activeLaunchContext is null)
                return;

            var loadedFolders = Folders
                .Where(folder => folder.IsLoaded)
                .Select(folder => (folder.ProjectRootId, folder.FullPath))
                .OrderBy(folder => folder.FullPath.Count(character => character == Path.DirectorySeparatorChar))
                .ToArray();
            AddProjectRoot(_activeLaunchContext);
            foreach (var loadedFolder in loadedFolders)
            {
                var folder = Folders.FirstOrDefault(candidate =>
                    candidate.ProjectRootId == loadedFolder.ProjectRootId &&
                    ProjectContentPathPolicy.IsSamePath(candidate.FullPath, loadedFolder.FullPath));
                if (folder is not null)
                {
                    MergeFolderSnapshot(folder, ReadDirectoryLevel(folder));
                }
            }
            Changed?.Invoke();
        }

        public async Task RefreshAsync(CancellationToken cancellationToken = default)
        {
            if (_activeLaunchContext is null)
            {
                return;
            }

            var launchContext = _activeLaunchContext;
            var loadedFolders = Folders
                .Where(folder => folder.IsLoaded)
                .Select(folder => (folder.ProjectRootId, folder.FullPath))
                .OrderBy(folder => folder.FullPath.Count(character => character == Path.DirectorySeparatorChar))
                .ToArray();
            await MainThread.InvokeOnMainThreadAsync(() => AddProjectRoot(launchContext));
            foreach (var loadedFolder in loadedFolders)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var folderId = await MainThread.InvokeOnMainThreadAsync(() =>
                    Folders.FirstOrDefault(candidate =>
                        candidate.ProjectRootId == loadedFolder.ProjectRootId &&
                        ProjectContentPathPolicy.IsSamePath(candidate.FullPath, loadedFolder.FullPath))?.Id);
                if (folderId is not null)
                {
                    await EnsureFolderLoadedAsync(folderId.Value, cancellationToken).ConfigureAwait(false);
                }
            }
        }

        public async Task EnsureFolderLoadedAsync(
            int folderId,
            CancellationToken cancellationToken = default)
        {
            var generation = Interlocked.Read(ref _contentGeneration);
            var folder = await MainThread.InvokeOnMainThreadAsync(() =>
                Folders.FirstOrDefault(candidate => candidate.Id == folderId));
            if (folder is null || folder.IsLoaded)
            {
                return;
            }

            await _folderLoadLock.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                folder = await MainThread.InvokeOnMainThreadAsync(() =>
                    Folders.FirstOrDefault(candidate => candidate.Id == folderId));
                if (folder is null || folder.IsLoaded ||
                    generation != Interlocked.Read(ref _contentGeneration))
                {
                    return;
                }

                var snapshot = await Task.Run(
                    () => ReadDirectoryLevel(folder),
                    cancellationToken).ConfigureAwait(false);
                await MainThread.InvokeOnMainThreadAsync(() =>
                {
                    if (generation != Interlocked.Read(ref _contentGeneration))
                    {
                        return;
                    }
                    var liveFolder = Folders.FirstOrDefault(candidate => candidate.Id == folderId);
                    if (liveFolder is null || liveFolder.IsLoaded)
                    {
                        return;
                    }
                    MergeFolderSnapshot(liveFolder, snapshot);
                    Changed?.Invoke();
                });
            }
            finally
            {
                _folderLoadLock.Release();
            }
        }

        public async Task<AssetFolder?> ResolveFolderAsync(
            string folderPath,
            CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(folderPath))
            {
                return null;
            }

            var canonicalPath = ProjectContentPathPolicy.NormalizeRoot(folderPath);
            var rootFolder = await MainThread.InvokeOnMainThreadAsync(() =>
                Folders
                    .Where(folder =>
                        folder.ParentFolderId == -1 &&
                        ProjectContentPathPolicy.IsInsideRoot(
                            folder.FullPath,
                            canonicalPath))
                    .OrderByDescending(folder => folder.FullPath.Length)
                    .FirstOrDefault());
            if (rootFolder is null)
            {
                return null;
            }

            await EnsureAssetDirectoryLoadedAsync(
                    canonicalPath,
                    rootFolder.IsReadOnly,
                    cancellationToken)
                .ConfigureAwait(false);
            return await MainThread.InvokeOnMainThreadAsync(() =>
                Folders.FirstOrDefault(folder =>
                    folder.ProjectRootId == rootFolder.ProjectRootId &&
                    ProjectContentPathPolicy.IsSamePath(
                        folder.FullPath,
                        canonicalPath)));
        }

        public async Task<AssetFile?> ResolveAssetAsync(
            FileId fileId,
            CancellationToken cancellationToken = default)
        {
            if (fileId is null || fileId.IsEmpty())
            {
                return null;
            }

            var asset = await MainThread.InvokeOnMainThreadAsync(() =>
                Assets.TryGetValue(fileId, out var current) ? current : null);
            if (asset is null)
            {
                await Volatile.Read(ref _assetCacheIndexLoadTask)
                    .WaitAsync(cancellationToken)
                    .ConfigureAwait(false);
                asset = await MainThread.InvokeOnMainThreadAsync(() =>
                    Assets.TryGetValue(fileId, out var current) ? current : null);
            }

            if (asset is null)
            {
                return null;
            }

            var assetDirectory = asset.AssetInfo?.DirectoryName ??
                asset.Asset?.DirectoryName;
            if (string.IsNullOrWhiteSpace(assetDirectory))
            {
                return asset;
            }

            await EnsureAssetDirectoryLoadedAsync(
                    assetDirectory,
                    asset.IsReadOnly,
                    cancellationToken)
                .ConfigureAwait(false);
            return await MainThread.InvokeOnMainThreadAsync(() =>
                Assets.TryGetValue(fileId, out var current) ? current : asset);
        }

        async Task EnsureAssetDirectoryLoadedAsync(
            string assetDirectory,
            bool isReadOnly,
            CancellationToken cancellationToken)
        {
            var targetDirectory = ProjectContentPathPolicy.NormalizeRoot(
                assetDirectory);
            var rootFolder = await MainThread.InvokeOnMainThreadAsync(() =>
                Folders
                    .Where(folder =>
                        folder.ParentFolderId == -1 &&
                        folder.IsReadOnly == isReadOnly &&
                        ProjectContentPathPolicy.IsInsideRoot(
                            folder.FullPath,
                            targetDirectory))
                    .OrderByDescending(folder => folder.FullPath.Length)
                    .FirstOrDefault());
            if (rootFolder is null)
            {
                return;
            }

            var relativeDirectory = Path.GetRelativePath(
                rootFolder.FullPath,
                targetDirectory);
            var pathSegments = relativeDirectory == "."
                ? Array.Empty<string>()
                : relativeDirectory.Split(
                    [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
                    StringSplitOptions.RemoveEmptyEntries);
            var currentFolder = rootFolder;
            var currentPath = ProjectContentPathPolicy.NormalizeRoot(
                rootFolder.FullPath);
            foreach (var pathSegment in pathSegments)
            {
                cancellationToken.ThrowIfCancellationRequested();
                await EnsureFolderLoadedAsync(
                        currentFolder.Id,
                        cancellationToken)
                    .ConfigureAwait(false);

                currentPath = ProjectContentPathPolicy.NormalizeRoot(
                    Path.Combine(currentPath, pathSegment));
                currentFolder = await MainThread.InvokeOnMainThreadAsync(() =>
                    Folders.FirstOrDefault(folder =>
                        folder.ParentFolderId == currentFolder.Id &&
                        ProjectContentPathPolicy.IsSamePath(
                            folder.FullPath,
                            currentPath)));
                if (currentFolder is null)
                {
                    return;
                }
            }

            await EnsureFolderLoadedAsync(
                    currentFolder.Id,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        public void ResetForWorkspaceChange()
        {
            Interlocked.Increment(ref _workspaceEpoch);
            Interlocked.Increment(ref _contentGeneration);
            CancelAssetCacheIndexLoad();
            DisposeContentWatchers();
            Root = new ProjectRoot { Name = string.Empty, Id = ActiveProjectRootId };
            Folders = [];
            Assets = [];
            Files = [];
            CurrentProjectRootPath = string.Empty;
            _activeLaunchContext = null;
            _folderIdAllocator = new ProjectContentFolderIdAllocator();
            _visitedDirectories = new HashSet<string>(ProjectContentPathPolicy.PathComparer);
            _nextAssetId = 1;
            _assetOverrideCount = 0;
            Changed?.Invoke();
        }

        public void AddProjectRoot(EngineLaunchContext launchContext)
        {
            using var perfScope = EditorPerf.Scope("AssetsService.AddProjectRoot");

            ArgumentNullException.ThrowIfNull(launchContext);
            var launchContextChanged = _activeLaunchContext is null || _activeLaunchContext != launchContext;
            if (launchContextChanged)
            {
                Interlocked.Increment(ref _workspaceEpoch);
            }
            var contentGeneration = Interlocked.Increment(ref _contentGeneration);
            CancelAssetCacheIndexLoad();
            var requestedRoot = Path.GetFullPath(launchContext.ContentDirectory);
            if (!Directory.Exists(requestedRoot))
            {
                throw new DirectoryNotFoundException(
                    $"The active content directory does not exist: {requestedRoot}");
            }
            var normalizedRoot = ProjectContentPathPolicy.NormalizeRoot(requestedRoot);

            Folders = [];
            Assets = [];
            Files = [];
            _folderIdAllocator = new ProjectContentFolderIdAllocator();
            _visitedDirectories = new HashSet<string>(ProjectContentPathPolicy.PathComparer);
            _nextAssetId = 1;
            _assetOverrideCount = 0;

            CurrentProjectRootPath = normalizedRoot;
            CurrentProjectMode = launchContext.Mode;
            _activeLaunchContext = launchContext;

            Root = new ProjectRoot { Name = CurrentProjectRootPath, Id = 1 };
            if (CurrentProjectMode == EditorProjectMode.Engine)
            {
                if (!ProjectContentPathPolicy.IsSamePath(CurrentProjectRootPath, _engineContentDirectory))
                {
                    throw new InvalidOperationException(
                        $"Engine mode content root must be the engine Content directory: {_engineContentDirectory}");
                }

                var engineRoot = new ProjectRoot { Name = "Engine Content", Id = EngineProjectRootId };

                AddContentRootFolder(engineRoot, ProjectContentFolderIds.EngineContentRootId, _engineContentDirectory, isReadOnly: false);
            }
            else if (ProjectContentPathPolicy.IsSamePath(CurrentProjectRootPath, _engineContentDirectory))
            {
                var workspaceRoot = new ProjectRoot { Name = "Content", Id = ActiveProjectRootId };
                AddContentRootFolder(workspaceRoot, ProjectContentFolderIds.ContentRootId, CurrentProjectRootPath, isReadOnly: false);
            }
            else
            {
                var workspaceRoot = new ProjectRoot { Name = "Content", Id = ActiveProjectRootId };
                var engineRoot = new ProjectRoot { Name = "Engine Content", Id = EngineProjectRootId };

                AddContentRootFolder(workspaceRoot, ProjectContentFolderIds.ContentRootId, CurrentProjectRootPath, isReadOnly: false);
                AddContentRootFolder(engineRoot, ProjectContentFolderIds.EngineContentRootId, _engineContentDirectory, isReadOnly: true);
            }

            if (_assetOverrideCount > 0)
                Console.WriteLine($"[AssetsService] Resolved {_assetOverrideCount} duplicate asset IDs; workspace content takes precedence when present.");

            if (launchContextChanged)
            {
                ConfigureContentWatchers(launchContext);
            }

            Changed?.Invoke();
            QueueAssetCacheIndexLoad(launchContext, contentGeneration);
        }

        void QueueAssetCacheIndexLoad(
            EngineLaunchContext launchContext,
            long contentGeneration)
        {
            var cancellation = new CancellationTokenSource();
            var previous = Interlocked.Exchange(
                ref _assetCacheIndexLoadCancellation,
                cancellation);
            previous?.Cancel();
            var loadTask = LoadAssetCacheIndexAsync(
                launchContext,
                contentGeneration,
                cancellation);
            Volatile.Write(ref _assetCacheIndexLoadTask, loadTask);
        }

        async Task LoadAssetCacheIndexAsync(
            EngineLaunchContext launchContext,
            long contentGeneration,
            CancellationTokenSource cancellation)
        {
            try
            {
                var result = await Task.Run(
                    () => _assetCacheIndexStore.Load(
                        launchContext.AssetCacheFilePath,
                        launchContext.WorkspaceIdentity,
                        cancellation.Token),
                    cancellation.Token).ConfigureAwait(false);
                if (!result.Succeeded)
                {
                    if (result.Status is not AssetCacheIndexStatus.Missing)
                    {
                        Console.WriteLine($"[AssetsService] {result.Diagnostic}");
                    }
                    return;
                }

                await MainThread.InvokeOnMainThreadAsync(() =>
                {
                    if (cancellation.IsCancellationRequested ||
                        contentGeneration != Interlocked.Read(ref _contentGeneration) ||
                        _activeLaunchContext != launchContext)
                    {
                        return;
                    }

                    MergeAssetCacheIndex(result.Entries!);
                    Changed?.Invoke();
                });
            }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
            {
            }
            catch (Exception exception)
            {
                Console.WriteLine(
                    $"[AssetsService] Failed to load asset cache index: {exception.Message}");
            }
            finally
            {
                Interlocked.CompareExchange(
                    ref _assetCacheIndexLoadCancellation,
                    null,
                    cancellation);
                cancellation.Dispose();
            }
        }

        void CancelAssetCacheIndexLoad()
            => Interlocked.Exchange(
                ref _assetCacheIndexLoadCancellation,
                null)?.Cancel();

        void ConfigureContentWatchers(EngineLaunchContext launchContext)
        {
            DisposeContentWatchers();
            var epoch = WorkspaceEpoch;
            var roots = new HashSet<string>(ProjectContentPathPolicy.PathComparer)
            {
                ProjectContentPathPolicy.NormalizeRoot(launchContext.ContentDirectory),
                _engineContentDirectory
            };

            lock (_watcherLock)
            {
                foreach (var root in roots)
                {
                    if (!Directory.Exists(root))
                    {
                        continue;
                    }

                    var watcher = new FileSystemWatcher(root)
                    {
                        IncludeSubdirectories = true,
                        NotifyFilter = NotifyFilters.FileName |
                            NotifyFilters.DirectoryName |
                            NotifyFilters.LastWrite |
                            NotifyFilters.Size
                    };
                    watcher.Changed += (_, args) => ScheduleFilesystemReload(epoch, args.FullPath);
                    watcher.Created += (_, args) => ScheduleFilesystemReload(epoch, args.FullPath);
                    watcher.Deleted += (_, args) => ScheduleFilesystemReload(epoch, args.FullPath);
                    watcher.Renamed += (_, args) => ScheduleFilesystemReload(epoch, args.FullPath);
                    watcher.Error += (_, args) =>
                    {
                        Console.WriteLine($"[AssetsService] Content watcher failed for '{root}': {args.GetException().Message}");
                        ScheduleFilesystemReload(epoch, root);
                    };
                    watcher.EnableRaisingEvents = true;
                    _contentWatchers.Add(watcher);
                }
            }
        }

        void ScheduleFilesystemReload(long epoch, string changedPath)
        {
            if (epoch != WorkspaceEpoch || _activeLaunchContext is null)
            {
                return;
            }

            var cancellation = new CancellationTokenSource();
            var previous = Interlocked.Exchange(ref _pendingFilesystemReload, cancellation);
            previous?.Cancel();
            _ = ReloadAfterFilesystemDebounceAsync(epoch, changedPath, cancellation);
        }

        async Task ReloadAfterFilesystemDebounceAsync(
            long epoch,
            string changedPath,
            CancellationTokenSource cancellation)
        {
            try
            {
                await Task.Delay(250, cancellation.Token).ConfigureAwait(false);
                if (epoch != WorkspaceEpoch || _activeLaunchContext is null || !_engineService.IsRunning)
                {
                    return;
                }

                if (!await _engineService.RequestAssetReloadAsync(cancellation.Token).ConfigureAwait(false))
                {
                    Console.WriteLine($"[AssetsService] Native asset reload rejected filesystem change: {changedPath}");
                }
            }
            catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
            {
            }
            finally
            {
                Interlocked.CompareExchange(ref _pendingFilesystemReload, null, cancellation);
                cancellation.Dispose();
            }
        }

        void DisposeContentWatchers()
        {
            Interlocked.Exchange(ref _pendingFilesystemReload, null)?.Cancel();
            lock (_watcherLock)
            {
                foreach (var watcher in _contentWatchers)
                {
                    watcher.EnableRaisingEvents = false;
                    watcher.Dispose();
                }
                _contentWatchers.Clear();
            }
        }

        public void Dispose()
        {
            _engineService.OnAssetReloadCompleted -= HandleAssetReloadCompleted;
            CancelAssetCacheIndexLoad();
            DisposeContentWatchers();
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
                IsReadOnly = isReadOnly,
                IsLoaded = false,
                HasChildren = DirectoryMayContainAssets(rootPath)
            });
        }

        private sealed record FolderLoadSnapshot(
            IReadOnlyList<AssetFolder> Folders,
            IReadOnlyList<AssetFile> Files);

        private void MergeAssetCacheIndex(
            IReadOnlyList<AssetCacheIndexEntry> entries)
        {
            foreach (var entry in entries)
            {
                if (!TryResolveAssetIndexLocation(
                        entry,
                        out var projectRootId,
                        out var isReadOnly))
                {
                    continue;
                }

                var fileId = new FileId(entry.FileId);
                var sourceFile = new FileInfo(entry.SourcePath);
                var indexedFilename = Path.GetFileNameWithoutExtension(
                    entry.MetadataFilename);
                var metadataFile = new FileInfo(
                    Path.Combine(sourceFile.DirectoryName!, entry.MetadataFilename));
                if (Assets.TryGetValue(fileId, out var existing))
                {
                    if ((existing.AssetInfo is not null &&
                         ProjectContentPathPolicy.IsSamePath(
                             existing.AssetInfo.FullName,
                             metadataFile.FullName)) ||
                        !ProjectContentAssetResolutionPolicy.ShouldReplace(
                            existing.IsReadOnly,
                            isReadOnly))
                    {
                        continue;
                    }
                }

                var indexedAsset = CreateAssetFile(
                    entry.AssetInfoType,
                    Path.GetExtension(indexedFilename));
                indexedAsset.Id = _nextAssetId++;
                indexedAsset.DisplayName = indexedFilename;
                indexedAsset.FolderId = -1;
                indexedAsset.ProjectRootId = projectRootId;
                indexedAsset.AssetInfo = metadataFile;
                indexedAsset.Asset = sourceFile;
                indexedAsset.OwnsSourceFile = string.Equals(
                    metadataFile.FullName,
                    sourceFile.FullName + ".asset",
                    ProjectContentPathPolicy.PathComparison);
                indexedAsset.FileId = fileId;
                indexedAsset.Filename = new FileId(indexedFilename);
                indexedAsset.IsDirty = false;
                indexedAsset.IsReadOnly = isReadOnly;
                indexedAsset.IsMetadataLoaded = false;
                Assets[fileId] = indexedAsset;
            }
        }

        private bool TryResolveAssetIndexLocation(
            AssetCacheIndexEntry entry,
            out int projectRootId,
            out bool isReadOnly)
        {
            projectRootId = ActiveProjectRootId;
            isReadOnly = false;
            if (ProjectContentPathPolicy.IsInsideRoot(
                    CurrentProjectRootPath,
                    entry.SourcePath))
            {
                return true;
            }

            if (CurrentProjectMode == EditorProjectMode.Workspace &&
                ProjectContentPathPolicy.IsInsideRoot(
                    _engineContentDirectory,
                    entry.SourcePath))
            {
                projectRootId = EngineProjectRootId;
                isReadOnly = true;
                return true;
            }

            return false;
        }

        private FolderLoadSnapshot ReadDirectoryLevel(AssetFolder parentFolder)
        {
            var folders = new List<AssetFolder>();
            var files = new List<AssetFile>();
            var rootPath = parentFolder.ProjectRootId == EngineProjectRootId
                ? _engineContentDirectory
                : CurrentProjectRootPath;
            var directoryPath = ProjectContentPathPolicy.NormalizeRoot(parentFolder.FullPath);
            if (!Directory.Exists(directoryPath) ||
                !ProjectContentPathPolicy.IsInsideRoot(rootPath, directoryPath))
            {
                return new FolderLoadSnapshot(folders, files);
            }

            foreach (var directory in Directory.GetDirectories(directoryPath)
                .Order(ProjectContentPathPolicy.PathComparer))
            {
                if (ProjectContentInternalPathPolicy.IsTransactionDirectory(directory))
                {
                    continue;
                }
                var canonicalChildPath = ProjectContentPathPolicy.NormalizeRoot(directory);
                if (!ProjectContentPathPolicy.IsInsideRoot(rootPath, canonicalChildPath))
                {
                    continue;
                }
                var relativeDirectoryPath = Path.GetRelativePath(rootPath, canonicalChildPath);
                folders.Add(new AssetFolder
                {
                    ProjectRootId = parentFolder.ProjectRootId,
                    Name = Path.GetFileName(canonicalChildPath),
                    Id = _folderIdAllocator.Allocate(
                        parentFolder.ProjectRootId,
                        relativeDirectoryPath,
                        useRootedFolderIds: true),
                    ParentFolderId = parentFolder.Id,
                    FullPath = canonicalChildPath,
                    IsReadOnly = parentFolder.IsReadOnly,
                    IsLoaded = false,
                    HasChildren = DirectoryMayContainAssets(canonicalChildPath)
                });
            }

            foreach (var file in Directory.GetFiles(directoryPath)
                .Order(ProjectContentPathPolicy.PathComparer))
            {
                if (!ProjectContentPathPolicy.IsInsideRoot(rootPath, file) ||
                    !string.Equals(Path.GetExtension(file), ".asset", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                try
                {
                    var asset = ReadAssetFile(
                        new FileInfo(file),
                        parentFolder.Id,
                        parentFolder.ProjectRootId,
                        parentFolder.IsReadOnly);
                    if (asset.FileId is not null && !asset.FileId.IsEmpty())
                    {
                        files.Add(asset);
                    }
                }
                catch (Exception exception)
                {
                    Console.WriteLine(exception);
                }
            }

            return new FolderLoadSnapshot(folders, files);
        }

        private void MergeFolderSnapshot(
            AssetFolder parentFolder,
            FolderLoadSnapshot snapshot)
        {
            foreach (var folder in snapshot.Folders)
            {
                if (!Folders.Any(candidate =>
                    candidate.ProjectRootId == folder.ProjectRootId &&
                    ProjectContentPathPolicy.IsSamePath(candidate.FullPath, folder.FullPath)))
                {
                    Folders.Add(folder);
                }
            }

            foreach (var file in snapshot.Files)
            {
                Files.Add(file);
                if (Assets.TryGetValue(file.FileId, out var existing))
                {
                    var bSameMetadata = existing.AssetInfo is not null &&
                        file.AssetInfo is not null &&
                        ProjectContentPathPolicy.IsSamePath(
                            existing.AssetInfo.FullName,
                            file.AssetInfo.FullName);
                    if (bSameMetadata || ProjectContentAssetResolutionPolicy.ShouldReplace(
                        existing.IsReadOnly,
                        file.IsReadOnly))
                    {
                        Assets[file.FileId] = file;
                    }
                    if (!bSameMetadata)
                    {
                        _assetOverrideCount++;
                    }
                }
                else
                {
                    Assets.Add(file.FileId, file);
                }
            }
            parentFolder.IsLoaded = true;
            parentFolder.HasChildren = snapshot.Folders.Count > 0 || snapshot.Files.Count > 0;
        }

        private static bool DirectoryMayContainAssets(string directoryPath)
        {
            try
            {
                return Directory.EnumerateFileSystemEntries(directoryPath).Any(path =>
                    Directory.Exists(path)
                        ? !ProjectContentInternalPathPolicy.IsTransactionDirectory(path)
                        : string.Equals(Path.GetExtension(path), ".asset", StringComparison.OrdinalIgnoreCase));
            }
            catch
            {
                return false;
            }
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

        public bool CanRenameAsset(AssetFile? assetFile)
            => assetFile?.OwnsSourceFile == true && CanModifyAsset(assetFile);

        public bool CanDuplicateAsset(AssetFile? assetFile)
            => assetFile?.Asset?.Exists == true && CanModifyAsset(assetFile);

        public bool CanModifyFolder(AssetFolder? folder)
        {
            if (folder is null || folder.IsReadOnly || IsContentRootFolder(folder) || string.IsNullOrWhiteSpace(folder.FullPath))
                return false;

            return Directory.Exists(folder.FullPath)
                && ProjectContentPathPolicy.IsInsideRoot(CurrentProjectRootPath, folder.FullPath)
                && !ProjectContentPathPolicy.IsSamePath(CurrentProjectRootPath, folder.FullPath);
        }

        public bool CanUseAsDestinationFolder(AssetFolder? folder)
            => TryGetWritableDestinationDirectory(folder, out _);

        public bool CanCreateFolder(AssetFolder? parentFolder)
            => TryGetWritableDestinationDirectory(parentFolder, out _);

        public bool CanMoveAsset(AssetFile? assetFile, AssetFolder? destinationFolder)
        {
            if (!CanModifyAsset(assetFile) || !TryGetWritableDestinationDirectory(destinationFolder, out var destinationDirectory))
                return false;

            var sourceDirectory = Path.GetDirectoryName(assetFile!.AssetInfo.FullName);
            return !string.IsNullOrWhiteSpace(sourceDirectory)
                && !ProjectContentPathPolicy.IsSamePath(sourceDirectory, destinationDirectory);
        }

        public bool CanMoveFolder(AssetFolder? folder, AssetFolder? destinationFolder)
        {
            if (!CanModifyFolder(folder) || !TryGetWritableDestinationDirectory(destinationFolder, out var destinationDirectory))
                return false;

            var sourceDirectory = folder!.FullPath;
            var sourceParent = Path.GetDirectoryName(sourceDirectory);
            return !string.IsNullOrWhiteSpace(sourceParent)
                && !ProjectContentPathPolicy.IsSamePath(sourceParent, destinationDirectory)
                && !ProjectContentPathPolicy.IsInsideRoot(sourceDirectory, destinationDirectory);
        }

        public static bool IsContentRootFolder(AssetFolder? folder)
            => folder?.Id is ProjectContentFolderIds.ContentRootId or ProjectContentFolderIds.EngineContentRootId;

        bool TryGetWritableDestinationDirectory(AssetFolder? folder, out string destinationDirectory)
        {
            destinationDirectory = folder is null ? CurrentProjectRootPath : GetFolderPath(folder);
            if (string.IsNullOrWhiteSpace(destinationDirectory) ||
                folder?.IsReadOnly == true ||
                !Directory.Exists(destinationDirectory))
            {
                return false;
            }

            return ProjectContentPathPolicy.IsInsideRoot(CurrentProjectRootPath, destinationDirectory);
        }

        static ProjectContentFileOperationResult FileOperationFailure(string error)
            => new(false, error, true, Array.Empty<string>());

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

        static string SerializePrefabAssetInfo(
            FileId fileId,
            string filename,
            List<InstanceId> externalRefs)
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
            using var writer = new StringWriter();
            yaml.Save(writer, false);
            return writer.ToString();
        }

        private AssetFile ReadAssetFile(FileInfo assetInfo, int parentFolderId, int projectRootId, bool isReadOnly)
        {
            var identity = ReadAssetMetadataIdentity(assetInfo);
            var declaredFilename = identity.Filename;
            if (!AssetSourcePathContract.TryResolve(
                    assetInfo.FullName,
                    declaredFilename,
                    out var sourceResolution,
                    out var sourceError))
            {
                throw new InvalidDataException($"{sourceError} Metadata: {assetInfo.FullName}");
            }
            FileInfo assetFile = new(sourceResolution.SourcePath);

            var extension = sourceResolution.AssetExtension;
            var assetInfoType = identity.AssetInfoType;
            AssetFile newAssetFile = CreateAssetFile(assetInfoType, extension);

            newAssetFile.Id = _nextAssetId++;
            newAssetFile.DisplayName = assetFile.Name;
            newAssetFile.FolderId = parentFolderId;
            newAssetFile.ProjectRootId = projectRootId;
            newAssetFile.AssetInfo = assetInfo;
            newAssetFile.Asset = assetFile;
            newAssetFile.OwnsSourceFile = sourceResolution.OwnsSourceFile;
            newAssetFile.FileId = new FileId(identity.FileId);
            newAssetFile.Filename = new FileId(identity.Filename);
            newAssetFile.IsDirty = false;
            newAssetFile.IsReadOnly = isReadOnly;
            newAssetFile.IsMetadataLoaded = false;

            return newAssetFile;
        }

        private static AssetFile CreateAssetFile(
            string assetInfoType,
            string extension)
        {
            var engineTypes = MauiProgram.GetService<EngineService>()?.EngineTypes;
            AssetType assetType = null;
            if (!string.IsNullOrEmpty(assetInfoType))
            {
                engineTypes?.AssetTypes?.TryGetValue(assetInfoType, out assetType);
            }
            if (assetType == null)
            {
                engineTypes?.AssetTypesByExtension?.TryGetValue(
                    extension.TrimStart('.').ToLowerInvariant(),
                    out assetType);
            }
            if (string.IsNullOrEmpty(assetInfoType) && assetType != null)
            {
                assetInfoType = assetType.Name;
            }

            AssetFile assetFile = assetInfoType switch
            {
                "Sailor::TextureAssetInfo" => new TextureFile(),
                "Sailor::ModelAssetInfo" => new ModelFile(),
                "Sailor::AnimationAssetInfo" => new AnimationFile(),
                "Sailor::AnimationControllerAssetInfo" => new AnimationControllerFile(),
                "Sailor::AnimationSetAssetInfo" => new AnimationSetFile(),
                "Sailor::PrefabAssetInfo" => new PrefabFile(),
                "Sailor::WorldPrefabAssetInfo" => new WorldFile(),
                "Sailor::ShaderAssetInfo" when string.Equals(
                    extension,
                    ".glsl",
                    StringComparison.OrdinalIgnoreCase) => new ShaderLibraryFile(),
                "Sailor::ShaderAssetInfo" => new ShaderFile(),
                "Sailor::MaterialAssetInfo" => new MaterialFile(),
                "Sailor::FrameGraphAssetInfo" => new FrameGraphFile(),
                _ => CreateAssetFileByExtension(extension)
            };
            assetFile.AssetInfoTypeName = assetInfoType;
            assetFile.AssetType = assetType;
            return assetFile;
        }

        private sealed record AssetMetadataIdentity(
            string FileId,
            string Filename,
            string AssetInfoType);

        private static AssetMetadataIdentity ReadAssetMetadataIdentity(FileInfo assetInfo)
        {
            using var reader = new StreamReader(assetInfo.FullName);
            var yaml = new YamlStream();
            yaml.Load(reader);
            if (yaml.Documents.Count == 0 ||
                yaml.Documents[0].RootNode is not YamlMappingNode root)
            {
                throw new InvalidDataException($"Asset metadata root must be a map: {assetInfo.FullName}");
            }

            static string ReadScalar(YamlMappingNode root, string name)
                => root.Children.TryGetValue(new YamlScalarNode(name), out var node) &&
                    node is YamlScalarNode scalar
                    ? scalar.Value ?? string.Empty
                    : string.Empty;

            var fileId = ReadScalar(root, "fileId");
            var filename = ReadScalar(root, "filename");
            if (string.IsNullOrWhiteSpace(fileId) || string.IsNullOrWhiteSpace(filename))
            {
                throw new InvalidDataException($"Asset metadata requires fileId and filename: {assetInfo.FullName}");
            }

            return new AssetMetadataIdentity(
                fileId,
                filename,
                ReadScalar(root, "assetInfoType"));
        }

        private static AssetFile CreateAssetFileByExtension(string extension) => extension switch
        {
            ".png" or ".jpg" or ".tga" or ".bmp" or ".dds" or ".hdr" => new TextureFile(),
            ".obj" or ".gltf" or ".glb" => new ModelFile(),
            ".anim" => new AnimationFile(),
            ".animcontroller" => new AnimationControllerFile(),
            ".animset" => new AnimationSetFile(),
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
                if (ProjectContentInternalPathPolicy.IsTransactionDirectory(directory))
                    continue;

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
