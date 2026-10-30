using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Content;
using SailorEditor.Controls;
using SailorEditor.Services;
using SailorEditor.Scene;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;
using SailorEngine;
using System.Collections.ObjectModel;
using GameObject = SailorEditor.ViewModels.GameObject;

namespace SailorEditor.Views
{
    public partial class ContentFolderView : ContentView
    {
        const double IndentWidth = 14.0;
        const string RootRowId = "folder:root";

        enum ContentDropOperation
        {
            None,
            Copy,
            Move
        }

        readonly AssetsService service;
        readonly ProjectContentStore contentStore;
        readonly ICommandDispatcher dispatcher;
        readonly IActionContextProvider contextProvider;
        readonly InspectorPendingEditCoordinator inspectorPendingEditCoordinator;
        readonly ObservableCollection<ContentListRow> visibleRows = [];
        readonly HashSet<int> expandedFolderIds = [];
        readonly Dictionary<string, object> rowModelsById = new(StringComparer.Ordinal);
        readonly SelectionService selectionService;
        readonly HashSet<Border> rowBorders = [];
        readonly List<string> selectedContentIds = [];
        bool isRootExpanded = true;
        int contentSelectionRequests;
        int contentSelectionVersion;
        bool selectingContent => contentSelectionRequests > 0;
        string? selectionAnchorId;
        long contentWorkspaceEpoch;

        public ContentFolderView()
        {
            InitializeComponent();

            service = MauiProgram.GetService<AssetsService>();
            contentStore = MauiProgram.GetService<ProjectContentStore>();
            dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            contextProvider = MauiProgram.GetService<IActionContextProvider>();
            inspectorPendingEditCoordinator =
                MauiProgram.GetService<InspectorPendingEditCoordinator>();

            if (!string.IsNullOrEmpty(contentStore.State.Filter))
            {
                contentStore.SetFilter(string.Empty);
            }

            ContentList.ItemsSource = visibleRows;
            ContentList.ItemTemplate = CreateItemTemplate();

            var rootDropGesture = new DropGestureRecognizer();
            rootDropGesture.DragOver += (_, e) => ApplyContentDropOperation(e, null);
            rootDropGesture.Drop += (_, e) =>
                RunContentUiAction(
                    () => HandleContentDrop(e, null),
                    "Drop Content item");
            ContentDropSurface.GestureRecognizers.Add(rootDropGesture);

            contentStore.ProjectionChanged += PopulateRows;

            selectionService = MauiProgram.GetService<SelectionService>();
            selectionService.OnSelectAssetAction += SelectAssetFile;
            selectionService.PropertyChanged += (_, args) =>
            {
                if (!selectingContent && args.PropertyName == nameof(SelectionService.SelectedItem))
                {
                    SynchronizeAssetSelection();
                    PopulateRows(contentStore.Projection);
                }
            };

            PopulateRows(contentStore.Projection);
        }

        async void SelectAssetFile(ObservableObject obj)
        {
            if (selectingContent)
            {
                return;
            }
            SynchronizeAssetSelection();
            if (obj is not AssetFile file || file.FileId is null || file.FileId.IsEmpty())
            {
                PopulateRows(contentStore.Projection);
                return;
            }

            try
            {
                var resolved = await service.ResolveAssetAsync(file.FileId);
                var selected = selectionService.SelectedItem as AssetFile;
                if (resolved is null || selected?.FileId is null || !selected.FileId.Equals(file.FileId))
                {
                    return;
                }
                EnsureFolderVisible(resolved.FolderId);
                contentStore.SelectAsset(resolved);
                PopulateRows(contentStore.Projection);
            }
            catch (Exception exception)
            {
                Console.WriteLine($"[ContentFolderView] Failed to reveal asset '{file.FileId}': {exception.Message}");
            }
        }

        void SynchronizeAssetSelection()
        {
            var assets = selectionService.SelectedItems.OfType<AssetFile>().ToArray();
            var previousAssets = selectedContentIds.Where(rowModelsById.ContainsKey)
                .Select(id => rowModelsById[id]).OfType<AssetFile>()
                .Select(asset => asset.FileId.Value).ToHashSet(StringComparer.Ordinal);
            var keepFolders = selectionService.SelectedItems.All(item => item is AssetFile) &&
                previousAssets.SetEquals(assets.Select(asset => asset.FileId.Value));
            if (keepFolders)
            {
                for (var index = 0; index < selectedContentIds.Count; index++)
                {
                    var previousId = selectedContentIds[index];
                    if (rowModelsById.TryGetValue(previousId, out var model) && model is AssetFile oldAsset)
                    {
                        var asset = assets.FirstOrDefault(item => item.FileId.Equals(oldAsset.FileId));
                        if (asset is not null)
                        {
                            selectedContentIds[index] = AssetRowId(GetAssetPath(asset));
                            if (selectionAnchorId == previousId)
                            {
                                selectionAnchorId = selectedContentIds[index];
                            }
                        }
                    }
                }
                return;
            }
            selectedContentIds.Clear();
            selectedContentIds.AddRange(assets.Select(asset => AssetRowId(GetAssetPath(asset)))
                .Distinct(StringComparer.Ordinal));
            selectionAnchorId ??= selectedContentIds.LastOrDefault();
        }

        async Task SelectRow(ContentListRow row, bool preserveSelected = false, bool useModifiers = true)
        {
            if (!rowModelsById.TryGetValue(row.Id, out var model))
            {
                return;
            }
            var modifiers = useModifiers ? EditorInputModifiers.Current : default;
            var selection = ListSelectionPolicy.Select(
                visibleRows.Select(item => item.Id).ToArray(), selectedContentIds, row.Id,
                selectionAnchorId, modifiers.Shift, modifiers.Additive, preserveSelected);
            selectionAnchorId = selection.AnchorId;
            selectedContentIds.Clear();
            selectedContentIds.AddRange(selection.SelectedIds);
            var requestVersion = ++contentSelectionVersion;
            var selectedAssets = GetSelectedContentModels().OfType<ObservableObject>().ToArray();
            contentSelectionRequests++;
            try
            {
                await selectionService.SelectObjectsAsync(selectedAssets);
                if (requestVersion != contentSelectionVersion)
                {
                    return;
                }
                if (!selectionService.SelectedItems.OfType<AssetFile>().Select(asset => asset.FileId.Value)
                    .SequenceEqual(selectedAssets.OfType<AssetFile>().Select(asset => asset.FileId.Value)) ||
                    selectionService.SelectedItems.Any(item => item is not AssetFile))
                {
                    SynchronizeAssetSelection();
                    return;
                }
                if (selection.PrimaryId is not null && rowModelsById.TryGetValue(selection.PrimaryId, out var primary))
                {
                    switch (primary)
                    {
                        case AssetFile assetFile:
                            contentStore.SelectAsset(assetFile);
                            break;
                        case AssetFolder folder:
                            contentStore.SelectFolder(folder.Id);
                            break;
                        case ProjectContentFolderItem:
                            contentStore.SelectFolder(null);
                            break;
                    }
                }
            }
            finally
            {
                contentSelectionRequests--;
                PopulateRows(contentStore.Projection);
            }
        }

        IReadOnlyList<object> GetSelectedContentModels(object? contextItem = null, bool rootsOnly = false)
        {
            var dirtyAssets = selectionService.SelectedItems.OfType<AssetFile>().Where(asset => asset.IsDirty)
                .ToDictionary(asset => asset.FileId.Value, StringComparer.Ordinal);
            var selected = selectedContentIds.Where(rowModelsById.ContainsKey)
                .Select(id => rowModelsById[id])
                .Select(item => item is AssetFile asset && dirtyAssets.TryGetValue(asset.FileId.Value, out var dirty)
                    ? dirty : item).ToArray();
            if (contextItem is not null && !selected.Any(item => ContentIdentity(item) == ContentIdentity(contextItem)))
            {
                selected = [contextItem];
            }
            return rootsOnly ? EditorDragDrop.GetSelectionRoots(selected) : selected;
        }

        static string ContentIdentity(object model) => model switch
        {
            AssetFile asset => AssetRowId(GetAssetPath(asset)),
            AssetFolder folder => FolderRowId(folder.Id),
            _ => RootRowId
        };

        async Task HandleContentDrop(DropEventArgs e, object target)
        {
            if (e.Handled)
            {
                return;
            }

            if (!e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var source))
            {
                return;
            }

            if (target is AssetFolder { IsReadOnly: true } or AssetFile { IsReadOnly: true })
            {
                e.Handled = true;
                return;
            }

            if (!EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out var requiresConfirmation) || command is null)
            {
                if (target is not null)
                {
                    e.Handled = true;
                }
                return;
            }

            var sourceItems = source is EditorSelectionDragPayload selectedPayload ? selectedPayload.Items : [source];
            var movingContent = sourceItems.All(item => item is AssetFile or AssetFolder);
            var originalSelection = movingContent ? GetSelectedContentModels(sourceItems[^1]) : [];
            var originalWorkspaceEpoch = selectionService.WorkspaceEpoch;
            var originalAnchor = selectionAnchorId is not null && rowModelsById.TryGetValue(selectionAnchorId, out var anchor)
                ? anchor : null;
            var folderPaths = sourceItems.OfType<AssetFolder>().ToDictionary(folder => folder.FullPath,
                folder => Path.Combine(service.GetFolderPath(target as AssetFolder), folder.Name), ProjectContentPathPolicy.PathComparer);
            e.Handled = true;
            await Task.Yield();

            if (requiresConfirmation && target is PrefabFile prefab && source is GameObject gameObject)
            {
                var overwrite = await Application.Current.MainPage.DisplayAlert(
                    "Overwrite prefab",
                    $"Overwrite {prefab.DisplayName} with {gameObject.DisplayName}?",
                    "Overwrite",
                    "Cancel");

                if (!overwrite)
                {
                    return;
                }
            }

            if (source is GameObject || source is EditorSelectionDragPayload payload && payload.Items.Any(item => item is GameObject))
            {
                if (!await inspectorPendingEditCoordinator
                        .CommitPendingChangesAsync())
                {
                    await Application.Current.MainPage.DisplayAlert(
                        "Drop failed",
                        "Pending Inspector changes could not be committed.",
                        "OK");
                    return;
                }

                if (!EditorDragDrop.TryCreateContentDropCommand(
                        source,
                        target,
                        out command,
                        out _) ||
                    command is null)
                {
                    await Application.Current.MainPage.DisplayAlert(
                        "Drop failed",
                        "The GameObject is no longer available for prefab creation.",
                        "OK");
                    return;
                }
            }

            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(ContentFolderView)));
            var result = await dispatcher.DispatchAsync(command, context);
            if (movingContent)
            {
                await RestoreContentSelectionAsync(originalSelection, originalAnchor, folderPaths, originalWorkspaceEpoch);
            }
            else
            {
                var createdAssets = new List<ObservableObject>();
                foreach (var id in EnumerateCreatedAssetIds(result.Value))
                {
                    var asset = await service.ResolveAssetAsync(id);
                    if (asset is not null)
                    {
                        createdAssets.Add(asset);
                    }
                }
                if (createdAssets.Count > 0)
                {
                    await selectionService.SelectObjectsAsync(createdAssets);
                }
            }
            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Drop failed",
                    result.Message ?? "Unable to complete the Content operation.",
                    "OK");
            }
        }

        ContentDropOperation GetContentDropOperation(DragEventArgs e, object target)
        {
            if (!e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var source) ||
                !EditorDragDrop.TryCreateContentDropCommand(source, target, out _, out _))
            {
                return ContentDropOperation.None;
            }
            var items = source is EditorSelectionDragPayload selection ? selection.Items : [source];
            var operations = items.Select(item => item switch
            {
                AssetFile asset when service.CanMoveAsset(asset, target as AssetFolder) => ContentDropOperation.Move,
                AssetFolder folder when service.CanMoveFolder(folder, target as AssetFolder) => ContentDropOperation.Move,
                GameObject => ContentDropOperation.Copy,
                _ => ContentDropOperation.None
            }).Distinct().ToArray();
            return operations.Length == 1 ? operations[0] : ContentDropOperation.None;
        }

        void ApplyContentDropOperation(DragEventArgs e, object target)
        {
            var operation = GetContentDropOperation(e, target);
            e.AcceptedOperation = operation == ContentDropOperation.None
                ? DataPackageOperation.None
                : DataPackageOperation.Copy;

#if MACCATALYST || IOS
            if (operation == ContentDropOperation.Move && e.PlatformArgs is not null)
            {
                e.PlatformArgs.SetDropProposal(new UIKit.UIDropProposal(UIKit.UIDropOperation.Move));
            }
#elif WINDOWS
            if (e.PlatformArgs is not null)
            {
                e.PlatformArgs.DragEventArgs.AcceptedOperation = operation switch
                {
                    ContentDropOperation.Move => Windows.ApplicationModel.DataTransfer.DataPackageOperation.Move,
                    ContentDropOperation.Copy => Windows.ApplicationModel.DataTransfer.DataPackageOperation.Copy,
                    _ => Windows.ApplicationModel.DataTransfer.DataPackageOperation.None
                };
                e.PlatformArgs.Handled = true;
            }
#endif
        }

        async Task OpenWorldWithConfirmation(WorldFile worldFile)
        {
            var page = Application.Current?.Windows?.FirstOrDefault()?.Page ?? Application.Current?.MainPage;
            if (page is null)
            {
                return;
            }

            var open = await page.DisplayAlert(
                "Open world",
                $"Open {worldFile.DisplayName} as the current editor world?",
                "Open",
                "Cancel");

            if (!open)
            {
                return;
            }

            var worldService = MauiProgram.GetService<WorldService>();
            var history = MauiProgram.GetService<ICommandHistoryService>();
            if (history.CanUndo)
            {
                var save = await page.DisplayAlert(
                    "Save current world",
                    "The current world has editor changes. Save it before opening another world?",
                    "Save",
                    "Discard");

                if (save)
                {
                    var saveResult = await worldService.SaveCurrentWorldAsync(confirmExisting: false);
                    if (saveResult.Outcome == SceneSaveOutcome.Cancelled)
                    {
                        return;
                    }
                    if (!saveResult.Succeeded)
                    {
                        await page.DisplayAlert("Save failed", saveResult.Error ?? "Unable to save the current world asset.", "OK");
                        return;
                    }
                }
            }

            if (!await worldService.LoadWorldAsync(worldFile))
            {
                await page.DisplayAlert("Open world failed", $"Unable to open {worldFile.DisplayName}.", "OK");
            }
        }

        async Task<CommandResult> DispatchContentGroupAsync(string description, IEnumerable<IEditorCommand> commands, bool throwOnFailure = true)
        {
            if (!await inspectorPendingEditCoordinator.CommitPendingChangesAsync())
            {
                throw new InvalidOperationException("Pending Inspector changes could not be committed.");
            }
            var result = await dispatcher.DispatchAsync(EditorCommandBatch.Create(description, commands),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Menu, nameof(ContentFolderView))));
            if (!result.Succeeded && throwOnFailure)
            {
                throw new InvalidOperationException(result.Message ?? description + " failed.");
            }
            return result;
        }

        bool CanRenameSelection(object contextItem) => GetSelectedContentModels(contextItem, rootsOnly: true)
            .All(item => item switch
            {
                AssetFile asset => service.CanRenameAsset(asset),
                AssetFolder folder => service.CanModifyFolder(folder),
                _ => false
            });

        bool CanDeleteSelection(object contextItem) => GetSelectedContentModels(contextItem, rootsOnly: true)
            .All(item => item switch
            {
                AssetFile asset => service.CanModifyAsset(asset),
                AssetFolder folder => service.CanModifyFolder(folder),
                _ => false
            });

        bool CanDuplicateSelection(object contextItem) => GetSelectedContentModels(contextItem, rootsOnly: true)
            .All(item => item is AssetFile asset && service.CanDuplicateAsset(asset));

        async Task DuplicateSelectedContentAsync(object contextItem)
        {
            var selected = GetSelectedContentModels(contextItem, rootsOnly: true);
            if (!CanDuplicateSelection(contextItem))
            {
                throw new InvalidOperationException("The selection contains items that cannot be duplicated.");
            }
            var result = await DispatchContentGroupAsync("Duplicate Content items", selected.OfType<AssetFile>()
                .Select(asset => (IEditorCommand)new DuplicateAssetCommand(asset)), throwOnFailure: false);
            var created = new List<ObservableObject>();
            foreach (var id in EnumerateCreatedAssetIds(result.Value))
            {
                var asset = await service.ResolveAssetAsync(id);
                if (asset is not null)
                {
                    created.Add(asset);
                }
            }
            if (created.Count > 0)
            {
                await selectionService.SelectObjectsAsync(created);
            }
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(result.Message ?? "Duplicate Content items failed.");
            }
        }

        static IEnumerable<FileId> EnumerateCreatedAssetIds(object? value)
        {
            if (value is FileId id)
            {
                yield return id;
            }
            else if (value is string text && !string.IsNullOrWhiteSpace(text))
            {
                yield return new FileId(text);
            }
            else if (value is IEnumerable<CommandResult> results)
            {
                foreach (var result in results.Where(result => result.Succeeded))
                {
                    foreach (var created in EnumerateCreatedAssetIds(result.Value))
                    {
                        yield return created;
                    }
                }
            }
        }

        async Task RenameSelectedContentAsync(object contextItem)
        {
            var originalSelection = GetSelectedContentModels(contextItem);
            var originalWorkspaceEpoch = selectionService.WorkspaceEpoch;
            var originalAnchor = selectionAnchorId is not null && rowModelsById.TryGetValue(selectionAnchorId, out var anchor)
                ? anchor : null;
            var folderPaths = new Dictionary<string, string>(ProjectContentPathPolicy.PathComparer);
            var commands = new List<IEditorCommand>();
            foreach (var item in GetSelectedContentModels(contextItem, rootsOnly: true))
            {
                var name = item switch { AssetFile asset => asset.DisplayName, AssetFolder folder => folder.Name, _ => null };
                if (name is null)
                {
                    throw new InvalidOperationException("The selected root cannot be renamed.");
                }
                var newName = await Application.Current.MainPage.DisplayPromptAsync(
                    "Rename Content item", $"New name for {name}", "OK", "Cancel", initialValue: name);
                if (string.IsNullOrWhiteSpace(newName))
                {
                    return;
                }
                if (item is AssetFolder renamedFolder)
                {
                    folderPaths[renamedFolder.FullPath] = Path.Combine(Path.GetDirectoryName(renamedFolder.FullPath)!, newName);
                }
                commands.Add(item switch
                {
                    AssetFile asset => new RenameAssetCommand(asset, newName),
                    AssetFolder folder => new RenameFolderCommand(folder, newName),
                    _ => throw new InvalidOperationException("The selected item cannot be renamed.")
                });
            }
            try
            {
                await DispatchContentGroupAsync("Rename Content items", commands);
            }
            finally
            {
                await RestoreContentSelectionAsync(originalSelection, originalAnchor, folderPaths, originalWorkspaceEpoch);
            }
        }

        async Task RestoreContentSelectionAsync(
            IReadOnlyList<object> originalSelection,
            object? originalAnchor,
            IReadOnlyDictionary<string, string> folderPaths,
            long originalWorkspaceEpoch)
        {
            if (selectionService.WorkspaceEpoch != originalWorkspaceEpoch)
            {
                return;
            }
            async Task<object?> ResolveAsync(object? item)
            {
                if (item is AssetFile asset)
                {
                    return await ProjectContentSelectionRestorePolicy.ResolveAssetAsync(service, asset, folderPaths);
                }
                if (item is AssetFolder folder)
                {
                    var path = folder.FullPath;
                    foreach (var mapping in folderPaths)
                    {
                        if (ProjectContentPathPolicy.IsInsideRoot(mapping.Key, path))
                        {
                            path = ProjectContentFolderRebindPolicy.CreatePlan(path, mapping.Key, mapping.Value,
                                ProjectContentFolderMutationKind.MoveOrRename).SuccessPath!;
                            break;
                        }
                    }
                    return await service.ResolveFolderAsync(path) ?? await service.ResolveFolderAsync(folder.FullPath);
                }
                return item is ProjectContentFolderItem ? contentStore.Projection.Root : null;
            }
            var models = new List<object>();
            foreach (var original in originalSelection)
            {
                var resolved = await ResolveAsync(original);
                if (selectionService.WorkspaceEpoch != originalWorkspaceEpoch)
                {
                    return;
                }
                if (resolved is not null)
                {
                    models.Add(resolved);
                }
            }
            var liveAnchor = await ResolveAsync(originalAnchor);
            if (selectionService.WorkspaceEpoch != originalWorkspaceEpoch)
            {
                return;
            }
            selectedContentIds.Clear();
            selectedContentIds.AddRange(models.Select(ContentIdentity));
            selectionAnchorId = liveAnchor is null ? selectedContentIds.LastOrDefault() : ContentIdentity(liveAnchor);
            contentSelectionRequests++;
            try
            {
                await selectionService.SelectObjectsAsync(models.OfType<ObservableObject>());
                switch (models.LastOrDefault())
                {
                    case AssetFile asset:
                        contentStore.SelectAsset(asset);
                        break;
                    case AssetFolder folder:
                        contentStore.SelectFolder(folder.Id);
                        break;
                }
            }
            finally
            {
                contentSelectionRequests--;
                PopulateRows(contentStore.Projection);
            }
        }

        async Task DeleteSelectedContentAsync(object contextItem)
        {
            var selected = GetSelectedContentModels(contextItem, rootsOnly: true);
            var names = selected.Select(item => item switch
            {
                AssetFile asset => asset.DisplayName,
                AssetFolder folder => folder.Name,
                _ => throw new InvalidOperationException("The selected root cannot be deleted.")
            });
            var confirmed = await Application.Current.MainPage.DisplayAlert(
                "Delete Content items", $"Delete {string.Join(", ", names)}, including source files, related metadata and folder contents?",
                "Delete", "Cancel");
            if (!confirmed)
            {
                return;
            }
            await DispatchContentGroupAsync("Delete Content items", selected.Select(item => item switch
            {
                AssetFile asset => (IEditorCommand)new DeleteAssetCommand(asset),
                AssetFolder folder => new DeleteFolderCommand(folder),
                _ => throw new InvalidOperationException("The selected item cannot be deleted.")
            }));
        }

        async Task CreateFolder(AssetFolder? parentFolder)
        {
            var folderName = await Application.Current.MainPage.DisplayPromptAsync(
                "Create folder",
                "Folder name",
                "Create",
                "Cancel",
                initialValue: "New Folder");

            if (string.IsNullOrWhiteSpace(folderName))
            {
                return;
            }

            var result = await dispatcher.DispatchAsync(
                new CreateFolderCommand(parentFolder, folderName),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(CreateFolder))));

            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Create folder failed",
                    result.Message ?? "Unable to create the folder.",
                    "OK");
                return;
            }

            if (result.Value is int createdFolderId)
            {
                EnsureFolderVisible(createdFolderId);
                PopulateRows(contentStore.Projection);
            }
        }

        async Task CreateAnimationAsset(
            AssetFolder? parentFolder,
            bool createSet)
        {
            var result = await dispatcher.DispatchAsync(
                new CreateAnimationAssetCommand(parentFolder, createSet),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.Panel,
                        nameof(CreateAnimationAsset))));
            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Create animation asset failed",
                    result.Message ?? "Unable to create the animation asset.",
                    "OK");
                return;
            }

            if (result.Value is FileId fileId &&
                service.Assets.TryGetValue(fileId, out var asset))
            {
                MauiProgram.GetService<SelectionService>()
                    .SelectObject(asset, force: true);
            }
        }

        async Task ToggleFolder(ContentListRow row)
        {
            if (row.Id == RootRowId)
            {
                isRootExpanded = !isRootExpanded;
                PopulateRows(contentStore.Projection);
                return;
            }

            if (!TryParseFolderId(row.Id, out var folderId))
            {
                return;
            }

            if (expandedFolderIds.Contains(folderId))
            {
                expandedFolderIds.Remove(folderId);
            }
            else
            {
                await service.EnsureFolderLoadedAsync(folderId);
                expandedFolderIds.Add(folderId);
            }

            PopulateRows(contentStore.Projection);
        }

        void PopulateRows(ProjectContentProjection projection)
        {
            if (contentWorkspaceEpoch != contentStore.WorkspaceEpoch)
            {
                contentWorkspaceEpoch = contentStore.WorkspaceEpoch;
                selectedContentIds.Clear();
                selectionAnchorId = null;
            }
            var selectedAssetIds = selectedContentIds.Where(rowModelsById.ContainsKey)
                .Select(id => (Id: id, Model: rowModelsById[id])).Where(item => item.Model is AssetFile)
                .ToDictionary(item => item.Id, item => ((AssetFile)item.Model).FileId.Value, StringComparer.Ordinal);
            var selectedFolderPaths = selectedContentIds.Where(rowModelsById.ContainsKey)
                .Select(id => (Id: id, Model: rowModelsById[id])).Where(item => item.Model is AssetFolder)
                .ToDictionary(item => item.Id, item => ((AssetFolder)item.Model).FullPath, StringComparer.Ordinal);
            var anchorFolderPath = selectionAnchorId is not null && rowModelsById.TryGetValue(selectionAnchorId, out var anchorModel) &&
                anchorModel is AssetFolder anchorFolder ? anchorFolder.FullPath : null;
            rowModelsById.Clear();
            rowModelsById[RootRowId] = projection.Root;
            foreach (var folder in service.Folders)
            {
                rowModelsById[FolderRowId(folder.Id)] = folder;
            }
            foreach (var asset in service.Files)
            {
                rowModelsById[AssetRowId(GetAssetPath(asset))] = asset;
            }
            for (var index = 0; index < selectedContentIds.Count; index++)
            {
                var previousId = selectedContentIds[index];
                if (selectedAssetIds.TryGetValue(previousId, out var fileId))
                {
                    var liveAsset = service.Files.FirstOrDefault(asset => asset.FileId?.Value == fileId);
                    if (liveAsset is not null)
                    {
                        selectedContentIds[index] = AssetRowId(GetAssetPath(liveAsset));
                        if (selectionAnchorId == previousId)
                        {
                            selectionAnchorId = selectedContentIds[index];
                        }
                    }
                }
                else if (selectedFolderPaths.TryGetValue(previousId, out var path))
                {
                    var liveFolder = service.Folders.FirstOrDefault(folder =>
                        string.Equals(folder.FullPath, path, ProjectContentPathPolicy.PathComparison));
                    if (liveFolder is not null)
                    {
                        selectedContentIds[index] = FolderRowId(liveFolder.Id);
                    }
                }
            }
            if (anchorFolderPath is not null)
            {
                var liveAnchor = service.Folders.FirstOrDefault(folder =>
                    string.Equals(folder.FullPath, anchorFolderPath, ProjectContentPathPolicy.PathComparison));
                if (liveAnchor is not null)
                {
                    selectionAnchorId = FolderRowId(liveAnchor.Id);
                }
            }
            selectedContentIds.RemoveAll(id => !rowModelsById.ContainsKey(id));
            if (selectionAnchorId is not null && !rowModelsById.ContainsKey(selectionAnchorId))
            {
                selectionAnchorId = selectedContentIds.LastOrDefault();
            }

            var foldersById = service.Folders.ToDictionary(x => x.Id);
            var assetsByFolder = service.Files
                .Where(x => x.FileId is not null && !x.FileId.IsEmpty())
                .GroupBy(x => x.FolderId)
                .ToDictionary(x => x.Key, x => x.OrderBy(asset => asset.DisplayName, StringComparer.OrdinalIgnoreCase).ToArray());
            var foldersByParent = service.Folders
                .GroupBy(x => x.ParentFolderId)
                .ToDictionary(x => x.Key, x => x.OrderBy(folder => folder.Name, StringComparer.OrdinalIgnoreCase).ToArray());

            var rows = new List<ContentListRow>();
            if (projection.IsRootVisible)
            {
                rowModelsById[RootRowId] = projection.Root;
                rows.Add(new ContentListRow(
                    RootRowId,
                    0,
                    projection.Root.Name,
                    "folder_open_24.png",
                    HasChildren(foldersByParent, assetsByFolder, -1),
                    isRootExpanded,
                    selectedContentIds.Contains(RootRowId)));

                if (isRootExpanded)
                {
                    AppendChildren(rows, foldersByParent, assetsByFolder, foldersById, -1, projection.TopLevelDepth, projection);
                }
            }
            else
            {
                AppendChildren(rows, foldersByParent, assetsByFolder, foldersById, -1, projection.TopLevelDepth, projection);
            }

            ContentRowReconciler.Reconcile(visibleRows, rows, row => row.Id);
            foreach (var border in rowBorders)
            {
                UpdateRowHighlight(border);
            }
        }

        void UpdateRowHighlight(Border border)
        {
            if (border.BindingContext is not ContentListRow row)
            {
                return;
            }
            var selected = selectedContentIds.Contains(row.Id);
            border.BackgroundColor = selected ? Color.FromArgb("#334C6FFF") : Colors.Transparent;
            border.Stroke = Color.FromArgb("#779DDB");
            border.StrokeThickness = selected && selectedContentIds.LastOrDefault() == row.Id ? 1 : 0;
        }

        void AppendChildren(
            List<ContentListRow> rows,
            IReadOnlyDictionary<int, AssetFolder[]> foldersByParent,
            IReadOnlyDictionary<int, AssetFile[]> assetsByFolder,
            IReadOnlyDictionary<int, AssetFolder> foldersById,
            int parentFolderId,
            int depth,
            ProjectContentProjection projection)
        {
            if (foldersByParent.TryGetValue(parentFolderId, out var folders))
            {
                foreach (var folder in folders)
                {
                    var id = FolderRowId(folder.Id);
                    var isExpanded = expandedFolderIds.Contains(folder.Id);
                    rowModelsById[id] = folder;
                    rows.Add(new ContentListRow(
                        id,
                        depth,
                        folder.Name,
                        folder.IsReadOnly ? "lock_24.png" : isExpanded ? "folder_open_24.png" : "folder_24.png",
                        HasChildren(foldersByParent, assetsByFolder, folder.Id) ||
                            (!folder.IsLoaded && folder.HasChildren),
                        isExpanded,
                        selectedContentIds.Contains(id)));

                    if (isExpanded)
                    {
                        AppendChildren(rows, foldersByParent, assetsByFolder, foldersById, folder.Id, depth + 1, projection);
                    }
                }
            }

            if (assetsByFolder.TryGetValue(parentFolderId, out var assets))
            {
                foreach (var asset in assets)
                {
                    var assetPath = GetAssetPath(asset);
                    var id = AssetRowId(assetPath);
                    rowModelsById[id] = asset;
                    rows.Add(new ContentListRow(
                        id,
                        depth,
                        asset.DisplayName,
                        GetAssetIcon(asset),
                        false,
                        false,
                        selectedContentIds.Contains(id)));
                }
            }
        }

        DataTemplate CreateItemTemplate()
        {
            return new DataTemplate(() =>
            {
                var expandLabel = new Label
                {
                    WidthRequest = 12,
                    HorizontalTextAlignment = TextAlignment.Center,
                    VerticalTextAlignment = TextAlignment.Center,
                    FontSize = 10,
                    Margin = new Thickness(0, 0, 4, 0),
                    TextColor = Color.FromArgb("#D7DCE2")
                };
                expandLabel.SetBinding(Label.TextProperty, nameof(ContentListRow.ExpandGlyph));
                var expandTap = new TapGestureRecognizer();
                expandTap.Tapped += (_, _) =>
                {
                    if (expandLabel.BindingContext is ContentListRow { HasChildren: true } row)
                    {
                        RunContentUiAction(
                            () => ToggleFolder(row),
                            "Expand Content folder");
                    }
                };
                expandLabel.GestureRecognizers.Add(expandTap);

                var icon = new ResourceImage
                {
                    WidthRequest = 16,
                    HeightRequest = 16,
                    Margin = new Thickness(0, 0, 6, 0),
                    VerticalOptions = LayoutOptions.Center
                };
                icon.SetBinding(ResourceImage.ResourceProperty, nameof(ContentListRow.Icon));

                var nameLabel = new Label
                {
                    VerticalTextAlignment = TextAlignment.Center,
                    LineBreakMode = LineBreakMode.TailTruncation,
                    TextColor = Color.FromArgb("#DDE3EA")
                };
                nameLabel.SetBinding(Label.TextProperty, nameof(ContentListRow.Label));

                var rowLayout = new HorizontalStackLayout
                {
                    Spacing = 0,
                    Padding = new Thickness(4, 2),
                    HeightRequest = 24,
                    VerticalOptions = LayoutOptions.Center,
                    HorizontalOptions = LayoutOptions.Fill
                };
                rowLayout.Children.Add(expandLabel);
                rowLayout.Children.Add(icon);
                rowLayout.Children.Add(nameLabel);

                var border = new Border
                {
                    StrokeThickness = 0,
                    Padding = 0,
                    Margin = new Thickness(2, 1),
                    Content = rowLayout
                };

                var dragGesture = new DragGestureRecognizer();
                dragGesture.DragStarting += (_, e) =>
                {
                    if (border.BindingContext is ContentListRow row &&
                        rowModelsById.TryGetValue(row.Id, out var model))
                    {
                        switch (model)
                        {
                            case AssetFile assetFile:
                                PrepareDragSelection(row);
                                var assetPayload = EditorDragDrop.CreateSelectionPayload(GetSelectedContentModels(), assetFile);
                                e.Data.Properties[EditorDragDrop.DragItemKey] = assetPayload;
#if WINDOWS
                                IEnumerable<AssetFile> viewportAssets = assetPayload is EditorSelectionDragPayload selection
                                    ? selection.Items.OfType<AssetFile>()
                                    : [assetFile];
                                if (EditorDragDrop.IsViewportAssetDrop(assetPayload) &&
                                    SceneViewportAssetDropPayload.TryCreate(
                                        viewportAssets.Select(asset => asset.FileId),
                                        selectionService.WorkspaceEpoch,
                                        out var payload))
                                {
                                    e.Data.Text = payload;
                                }
#endif
                                return;
                            case AssetFolder folder when service.CanModifyFolder(folder):
                                PrepareDragSelection(row);
                                e.Data.Properties[EditorDragDrop.DragItemKey] =
                                    EditorDragDrop.CreateSelectionPayload(GetSelectedContentModels(), folder);
                                return;
                        }
                    }

                    e.Cancel = true;
                };
                border.GestureRecognizers.Add(dragGesture);

                var dropGesture = new DropGestureRecognizer();
                dropGesture.DragOver += (_, e) =>
                {
                    object target = null;
                    if (border.BindingContext is ContentListRow row && rowModelsById.TryGetValue(row.Id, out var model))
                    {
                        target = model is ProjectContentFolderItem ? null : model;
                    }

                    ApplyContentDropOperation(e, target);
                };
                dropGesture.Drop += (_, e) =>
                {
                    object target = null;
                    if (border.BindingContext is ContentListRow row && rowModelsById.TryGetValue(row.Id, out var model))
                    {
                        target = model is ProjectContentFolderItem ? null : model;
                    }

                    RunContentUiAction(
                        () => HandleContentDrop(e, target),
                        "Drop Content item");
                };
                border.GestureRecognizers.Add(dropGesture);

                var singleTap = new TapGestureRecognizer { NumberOfTapsRequired = 1 };
                singleTap.Tapped += (_, _) =>
                {
                    if (border.BindingContext is ContentListRow row)
                    {
                        RunContentUiAction(
                            () => SelectRow(row),
                            "Select Content item");
                    }
                };
                border.GestureRecognizers.Add(singleTap);

                var doubleTap = new TapGestureRecognizer { NumberOfTapsRequired = 2 };
                doubleTap.Tapped += (_, _) =>
                {
                    if (border.BindingContext is not ContentListRow row || !rowModelsById.TryGetValue(row.Id, out var model))
                    {
                        return;
                    }

                    RunContentUiAction(
                        async () =>
                        {
                            switch (model)
                            {
                                case WorldFile worldFile:
                                    await OpenWorldWithConfirmation(worldFile);
                                    break;
                                case AssetFolder or ProjectContentFolderItem:
                                    await ToggleFolder(row);
                                    break;
                            }
                        },
                        "Open Content item");
                };
                border.GestureRecognizers.Add(doubleTap);

                var contextTap = new TapGestureRecognizer { Buttons = ButtonsMask.Secondary };
                contextTap.Tapped += (_, _) =>
                {
                    if (border.BindingContext is ContentListRow row && rowModelsById.TryGetValue(row.Id, out var model))
                    {
                        RunContentUiAction(async () =>
                        {
                            await SelectRow(row, preserveSelected: true, useModifiers: false);
                            ShowContextMenu(model);
                        }, "Select Content items");
                    }
                };
                border.GestureRecognizers.Add(contextTap);

                rowBorders.Add(border);
                border.BindingContextChanged += (_, _) =>
                {
                    if (border.BindingContext is ContentListRow row)
                    {
                        rowLayout.Margin = new Thickness(row.Depth * IndentWidth, 0, 0, 0);
                        UpdateRowHighlight(border);
                    }
                };

                return border;
            });
        }

        void PrepareDragSelection(ContentListRow row)
        {
            if (selectedContentIds.Contains(row.Id))
            {
                return;
            }
            selectedContentIds.Clear();
            selectedContentIds.Add(row.Id);
            selectionAnchorId = row.Id;
            foreach (var border in rowBorders)
            {
                UpdateRowHighlight(border);
            }
            RunContentUiAction(() => SelectRow(row, preserveSelected: true, useModifiers: false), "Select Content item");
        }

        void ShowContextMenu(object model)
        {
            var contextMenu = MauiProgram.GetService<EditorContextMenuService>();
            if (model is AssetFile assetFile)
            {
                var items = new List<EditorContextMenuItem>();
                if (service.CanReimportAsset(assetFile))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Reimport",
                        Command = CreateContentContextMenuCommand(
                            () => ReimportAsset(assetFile),
                            "Reimport asset")
                    });
                }

                if (service.CanModifyAsset(assetFile) &&
                    service.CanDuplicateAsset(assetFile))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Duplicate",
                        IsEnabled = CanDuplicateSelection(assetFile),
                        Command = CreateContentContextMenuCommand(
                            () => DuplicateSelectedContentAsync(assetFile),
                            "Duplicate asset")
                    });
                }

                if (service.CanModifyAsset(assetFile) &&
                    service.CanRenameAsset(assetFile))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Rename",
                        IsEnabled = CanRenameSelection(assetFile),
                        Command = CreateContentContextMenuCommand(
                            () => RenameSelectedContentAsync(assetFile),
                            "Rename asset")
                    });
                }

                if (service.CanModifyAsset(assetFile))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Delete",
                        IsDestructive = true,
                        IsEnabled = CanDeleteSelection(assetFile),
                        Command = CreateContentContextMenuCommand(
                            () => DeleteSelectedContentAsync(assetFile),
                            "Delete asset")
                    });
                }

                if (items.Count > 0)
                {
                    contextMenu.Show(items.ToArray());
                }
            }
            else if (model is AssetFolder folder)
            {
                var items = new List<EditorContextMenuItem>();
                if (service.CanRefreshFolder(folder))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Refresh",
                        Command = CreateContentContextMenuCommand(
                            () => RefreshFolder(folder),
                            "Refresh folder")
                    });
                }

                if (service.CanCreateFolder(folder))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Create Folder",
                        Command = CreateContentContextMenuCommand(
                            () => CreateFolder(folder),
                            "Create folder")
                    });
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Create Animation Controller",
                        Command = CreateContentContextMenuCommand(
                            () => CreateAnimationAsset(folder, false),
                            "Create animation controller")
                    });
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Create Animation Set",
                        Command = CreateContentContextMenuCommand(
                            () => CreateAnimationAsset(folder, true),
                            "Create animation set")
                    });
                }

                if (service.CanModifyFolder(folder))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Rename",
                        IsEnabled = CanRenameSelection(folder),
                        Command = CreateContentContextMenuCommand(
                            () => RenameSelectedContentAsync(folder),
                            "Rename folder")
                    });
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Delete",
                        IsDestructive = true,
                        IsEnabled = CanDeleteSelection(folder),
                        Command = CreateContentContextMenuCommand(
                            () => DeleteSelectedContentAsync(folder),
                            "Delete folder")
                    });
                }

                if (items.Count > 0)
                {
                    contextMenu.Show(items.ToArray());
                }
            }
            else if (model is ProjectContentFolderItem { IsRoot: true } rootFolder)
            {
                var items = new List<EditorContextMenuItem>();
                if (service.CanRefreshFolder(rootFolder))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Refresh",
                        Command = CreateContentContextMenuCommand(
                            () => RefreshFolder(rootFolder),
                            "Refresh folder")
                    });
                }

                if (service.CanCreateFolder(null))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Create Folder",
                        Command = CreateContentContextMenuCommand(
                            () => CreateFolder(null),
                            "Create folder")
                    });
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Create Animation Controller",
                        Command = CreateContentContextMenuCommand(
                            () => CreateAnimationAsset(null, false),
                            "Create animation controller")
                    });
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Create Animation Set",
                        Command = CreateContentContextMenuCommand(
                            () => CreateAnimationAsset(null, true),
                            "Create animation set")
                    });
                }

                if (items.Count > 0)
                {
                    contextMenu.Show(items.ToArray());
                }
            }
        }

        async Task ReimportAsset(AssetFile assetFile)
        {
            if (!await service.ReimportAssetAsync(assetFile))
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Reimport failed",
                    $"Unable to reimport {assetFile.DisplayName}.",
                    "OK");
            }
        }

        async Task RefreshFolder(AssetFolder? folder)
        {
            if (!await service.RefreshFolderAsync(folder))
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Refresh failed",
                    "Unable to refresh the Content folder.",
                    "OK");
            }
        }

        async Task RefreshFolder(ProjectContentFolderItem folder)
        {
            if (!await service.RefreshFolderAsync(folder))
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Refresh failed",
                    "Unable to refresh the Content folder.",
                    "OK");
            }
        }

        static Command CreateContentContextMenuCommand(
            Func<Task> action,
            string operation)
        {
            return new Command(
                () => RunContentUiAction(
                    action,
                    operation));
        }

        static void RunContentUiAction(
            Func<Task> action,
            string operation)
        {
            _ = ExecuteContentUiActionAsync(
                action,
                operation);
        }

        static async Task ExecuteContentUiActionAsync(
            Func<Task> action,
            string operation)
        {
            try
            {
                await action();
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine(
                    $"{operation} failed: {exception}");
                try
                {
                    var page =
                        Application.Current?.Windows
                            .FirstOrDefault()?.Page ??
                        Application.Current?.MainPage;
                    if (page is not null)
                    {
                        await page.DisplayAlert(
                            "Content operation failed",
                            exception.Message,
                            "OK");
                    }
                }
                catch (Exception reportingException)
                {
                    Console.Error.WriteLine(
                        $"Failed to report {operation} error: {reportingException}");
                }
            }
        }

        void EnsureFolderVisible(int folderId)
        {
            isRootExpanded = true;

            var foldersById = service.Folders.ToDictionary(x => x.Id);
            var currentFolderId = folderId;
            while (currentFolderId != -1 && foldersById.TryGetValue(currentFolderId, out var folder))
            {
                expandedFolderIds.Add(folder.Id);
                currentFolderId = folder.ParentFolderId;
            }
        }

        static bool HasChildren(
            IReadOnlyDictionary<int, AssetFolder[]> foldersByParent,
            IReadOnlyDictionary<int, AssetFile[]> assetsByFolder,
            int folderId)
            => (foldersByParent.TryGetValue(folderId, out var folders) && folders.Length > 0) ||
               (assetsByFolder.TryGetValue(folderId, out var assets) && assets.Length > 0);

        static string GetAssetIcon(AssetFile asset) => asset switch
        {
            TextureFile => "image_24.png",
            ModelFile => "box_24.png",
            AnimationFile => "film_24.png",
            PrefabFile => "blueprint.png",
            WorldFile => "globe_24.png",
            ShaderFile or ShaderLibraryFile => "script_24.png",
            MaterialFile => "color_swatch_24.png",
            FrameGraphFile => "application_sidebar_list_24.png",
            LandscapeVegetationFile => "document_24.png",
            GIProbesFile => "light_bulb_24.png",
            _ => "document_24.png"
        };

        static string FolderRowId(int folderId) => $"folder:{folderId}";

        static string AssetRowId(string assetPath) => $"asset:{assetPath}";

        static string GetAssetPath(AssetFile asset)
            => asset.AssetInfo?.FullName ?? asset.Asset?.FullName ?? $"asset-{asset.Id}";

        static bool TryParseFolderId(string id, out int folderId)
        {
            folderId = default;
            return id.StartsWith("folder:", StringComparison.Ordinal) &&
                int.TryParse(id["folder:".Length..], out folderId);
        }

    }

    public sealed record ContentListRow(string Id, int Depth, string Label, string Icon, bool HasChildren, bool IsExpanded, bool IsSelected)
    {
        public string ExpandGlyph => HasChildren ? (IsExpanded ? "−" : "+") : string.Empty;
    }
}
