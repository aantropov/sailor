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
        bool isRootExpanded = true;
        bool suppressSelectionChanged;

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
            ContentList.SelectionChanged += OnContentSelectionChanged;
            ContentList.ItemTemplate = CreateItemTemplate();

            var rootDropGesture = new DropGestureRecognizer();
            rootDropGesture.DragOver += (_, e) => ApplyContentDropOperation(e, null);
            rootDropGesture.Drop += (_, e) =>
                RunContentUiAction(
                    () => HandleContentDrop(e, null),
                    "Drop Content item");
            ContentDropSurface.GestureRecognizers.Add(rootDropGesture);

            contentStore.ProjectionChanged += PopulateRows;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectAssetAction += SelectAssetFile;

            PopulateRows(contentStore.Projection);
        }

        async void SelectAssetFile(ObservableObject obj)
        {
            if (obj is not AssetFile file || file.FileId is null || file.FileId.IsEmpty())
            {
                contentStore.SelectAsset(null);
                suppressSelectionChanged = true;
                try
                {
                    ContentList.SelectedItem = null;
                }
                finally
                {
                    suppressSelectionChanged = false;
                }
                return;
            }

            try
            {
                var resolved = await service.ResolveAssetAsync(file.FileId);
                var selected = MauiProgram.GetService<SelectionService>()
                    .SelectedItem as AssetFile;
                if (resolved is null ||
                    selected?.FileId is null ||
                    !selected.FileId.Equals(file.FileId))
                {
                    return;
                }

                EnsureFolderVisible(resolved.FolderId);
                contentStore.SelectAsset(resolved);
            }
            catch (Exception exception)
            {
                Console.WriteLine(
                    $"[ContentFolderView] Failed to reveal asset '{file.FileId}': {exception.Message}");
            }
        }

        void OnContentSelectionChanged(object sender, SelectionChangedEventArgs args)
        {
            if (suppressSelectionChanged)
            {
                return;
            }

            if (args.CurrentSelection.FirstOrDefault() is not ContentListRow row ||
                !rowModelsById.TryGetValue(row.Id, out var model))
            {
                return;
            }

            RunContentUiAction(
                () => SelectRow(row, false),
                "Select Content item");
        }

        async Task SelectRow(ContentListRow row, bool updateCollectionSelection)
        {
            if (!rowModelsById.TryGetValue(row.Id, out var model))
            {
                return;
            }

            if (updateCollectionSelection)
            {
                suppressSelectionChanged = true;
                try
                {
                    ContentList.SelectedItem = row;
                }
                finally
                {
                    suppressSelectionChanged = false;
                }
            }

            switch (model)
            {
                case AssetFile assetFile:
                    MauiProgram.GetService<SelectionService>().SelectObject(assetFile, force: true);
                    contentStore.SelectAsset(assetFile);
                    await dispatcher.DispatchAsync(
                        new OpenAssetCommand(assetFile),
                        contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(ContentFolderView))));
                    break;
                case AssetFolder folder:
                    contentStore.SelectFolder(folder.Id);
                    break;
                case ProjectContentFolderItem:
                    contentStore.SelectFolder(null);
                    break;
            }
        }

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

            if (command is CreatePrefabAssetCommand)
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
                    command is not CreatePrefabAssetCommand)
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
                !EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out _))
            {
                return ContentDropOperation.None;
            }

            return command switch
            {
                MoveAssetCommand when source is AssetFile assetFile && service.CanMoveAsset(assetFile, target as AssetFolder) => ContentDropOperation.Move,
                MoveFolderCommand when source is AssetFolder assetFolder && service.CanMoveFolder(assetFolder, target as AssetFolder) => ContentDropOperation.Move,
                CreatePrefabAssetCommand => ContentDropOperation.Copy,
                _ => ContentDropOperation.None
            };
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
                        return;
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

        async Task RenameAsset(AssetFile assetFile)
        {
            var newName = await Application.Current.MainPage.DisplayPromptAsync(
                "Rename asset",
                "New asset name",
                "OK",
                "Cancel",
                initialValue: assetFile.Asset?.Name ?? assetFile.DisplayName);

            if (string.IsNullOrWhiteSpace(newName))
            {
                return;
            }

            var result = await dispatcher.DispatchAsync(
                new RenameAssetCommand(assetFile, newName),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(RenameAsset))));

            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Rename failed",
                    result.Message ?? "Asset with this name already exists or the name is invalid.",
                    "OK");
            }
        }

        async Task DeleteAsset(AssetFile assetFile)
        {
            var confirmed = await Application.Current.MainPage.DisplayAlert(
                "Delete asset group",
                $"Delete {assetFile.DisplayName}, its source file, and all related .asset metadata?",
                "Delete",
                "Cancel");

            if (!confirmed)
                return;

            var result = await dispatcher.DispatchAsync(
                new DeleteAssetCommand(assetFile),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(DeleteAsset))));

            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert("Delete failed", result.Message ?? "Unable to delete asset.", "OK");
            }
        }

        async Task DuplicateAsset(AssetFile assetFile)
        {
            var result = await dispatcher.DispatchAsync(
                new DuplicateAssetCommand(assetFile),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(DuplicateAsset))));

            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Duplicate failed",
                    result.Message ?? "Unable to duplicate the asset group.",
                    "OK");
            }
        }

        async Task RenameFolder(AssetFolder folder)
        {
            var newName = await Application.Current.MainPage.DisplayPromptAsync(
                "Rename folder",
                "New folder name",
                "OK",
                "Cancel",
                initialValue: folder.Name);

            if (string.IsNullOrWhiteSpace(newName))
            {
                return;
            }

            var result = await dispatcher.DispatchAsync(
                new RenameFolderCommand(folder, newName),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(RenameFolder))));

            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Rename folder failed",
                    result.Message ?? "Unable to rename the folder.",
                    "OK");
            }
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

        async Task DeleteFolder(AssetFolder folder)
        {
            var confirmed = await Application.Current.MainPage.DisplayAlert(
                "Delete folder",
                $"Delete {folder.Name} and all of its contents?",
                "Delete",
                "Cancel");

            if (!confirmed)
                return;

            var result = await dispatcher.DispatchAsync(
                new DeleteFolderCommand(folder),
                contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(DeleteFolder))));

            if (!result.Succeeded)
            {
                await Application.Current.MainPage.DisplayAlert(
                    "Delete folder failed",
                    result.Message ?? "Unable to delete the folder.",
                    "OK");
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
            rowModelsById.Clear();

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
                    IsSelectedRoot(projection)));

                if (isRootExpanded)
                {
                    AppendChildren(rows, foldersByParent, assetsByFolder, foldersById, -1, projection.TopLevelDepth, projection);
                }
            }
            else
            {
                AppendChildren(rows, foldersByParent, assetsByFolder, foldersById, -1, projection.TopLevelDepth, projection);
            }

            var desiredSelectedRow = rows.FirstOrDefault(
                row => row.IsSelected);
            var currentSelectedRow =
                ContentList.SelectedItem as ContentListRow;
            var preservesCurrentSelection =
                currentSelectedRow is not null &&
                desiredSelectedRow is not null &&
                string.Equals(
                    currentSelectedRow.Id,
                    desiredSelectedRow.Id,
                    StringComparison.Ordinal) &&
                EqualityComparer<ContentListRow>.Default.Equals(
                    currentSelectedRow,
                    desiredSelectedRow) &&
                visibleRows.Contains(currentSelectedRow);
            suppressSelectionChanged = true;
            try
            {
                if (!preservesCurrentSelection &&
                    currentSelectedRow is not null)
                {
                    ContentList.SelectedItem = null;
                }

                ContentRowReconciler.Reconcile(
                    visibleRows,
                    rows,
                    row => row.Id);
                ContentList.SelectedItem =
                    visibleRows.FirstOrDefault(row => row.IsSelected);
            }
            finally
            {
                suppressSelectionChanged = false;
            }
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
                        projection.State.SelectedAssetPath is null
                            && projection.State.SelectedAssetFileId is null
                            && projection.State.CurrentFolderId == folder.Id));

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
                        string.Equals(projection.State.SelectedAssetPath, assetPath, ProjectContentPathPolicy.PathComparison)));
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
                                e.Data.Properties[EditorDragDrop.DragItemKey] = assetFile;
#if WINDOWS
                                if (EditorDragDrop.IsViewportAssetDrop(assetFile) &&
                                    SceneViewportAssetDropPayload.TryCreate(
                                        assetFile.FileId,
                                        out var payload))
                                {
                                    e.Data.Text = payload;
                                }
#endif
                                return;
                            case AssetFolder folder when service.CanModifyFolder(folder):
                                e.Data.Properties[EditorDragDrop.DragItemKey] = folder;
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
                            () => SelectRow(row, true),
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
                        ShowContextMenu(model);
                    }
                };
                border.GestureRecognizers.Add(contextTap);

                border.BindingContextChanged += (_, _) =>
                {
                    if (border.BindingContext is ContentListRow row)
                    {
                        rowLayout.Margin = new Thickness(row.Depth * IndentWidth, 0, 0, 0);
                        border.BackgroundColor = row.IsSelected ? Color.FromArgb("#334C6FFF") : Colors.Transparent;
                    }
                };

                return border;
            });
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
                        Command = CreateContentContextMenuCommand(
                            () => DuplicateAsset(assetFile),
                            "Duplicate asset")
                    });
                }

                if (service.CanModifyAsset(assetFile) &&
                    service.CanRenameAsset(assetFile))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Rename",
                        Command = CreateContentContextMenuCommand(
                            () => RenameAsset(assetFile),
                            "Rename asset")
                    });
                }

                if (service.CanModifyAsset(assetFile))
                {
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Delete",
                        IsDestructive = true,
                        Command = CreateContentContextMenuCommand(
                            () => DeleteAsset(assetFile),
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
                        Command = CreateContentContextMenuCommand(
                            () => RenameFolder(folder),
                            "Rename folder")
                    });
                    items.Add(new EditorContextMenuItem
                    {
                        Text = "Delete",
                        IsDestructive = true,
                        Command = CreateContentContextMenuCommand(
                            () => DeleteFolder(folder),
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

        static bool IsSelectedRoot(ProjectContentProjection projection)
            => projection.State.SelectedAssetPath is null
                && projection.State.SelectedAssetFileId is null
                && projection.State.CurrentFolderId is null;

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
