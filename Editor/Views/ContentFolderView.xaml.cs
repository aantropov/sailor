using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Content;
using SailorEditor.Controls;
using SailorEditor.Services;
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

        readonly AssetsService service;
        readonly ProjectContentStore contentStore;
        readonly ICommandDispatcher dispatcher;
        readonly IActionContextProvider contextProvider;
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

            if (!string.IsNullOrEmpty(contentStore.State.Filter))
            {
                contentStore.SetFilter(string.Empty);
            }

            ContentList.ItemsSource = visibleRows;
            ContentList.SelectionChanged += OnContentSelectionChanged;
            ContentList.ItemTemplate = CreateItemTemplate();

            var rootDropGesture = new DropGestureRecognizer();
            rootDropGesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Copy;
            rootDropGesture.Drop += async (_, e) => await HandleContentDrop(e, null);
            ContentDropSurface.GestureRecognizers.Add(rootDropGesture);

            contentStore.ProjectionChanged += PopulateRows;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectAssetAction += SelectAssetFile;

            PopulateRows(contentStore.Projection);
        }

        void SelectAssetFile(ObservableObject obj)
        {
            if (obj is not AssetFile file || file.FileId is null || file.FileId.IsEmpty())
            {
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

            EnsureFolderVisible(file.FolderId);
            contentStore.SelectAsset(file.FileId.Value);
        }

        async void OnContentSelectionChanged(object sender, SelectionChangedEventArgs args)
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

            await SelectRow(row, false);
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
                    contentStore.SelectAsset(assetFile.FileId?.Value);
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

            if (!EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out var requiresConfirmation) || command is null)
            {
                return;
            }

            if (requiresConfirmation && target is PrefabFile prefab && source is GameObject gameObject)
            {
                e.Handled = true;
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

            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(ContentFolderView)));
            e.Handled = (await dispatcher.DispatchAsync(command, context)).Succeeded;
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

                if (save && !worldService.SaveCurrentWorld())
                {
                    await page.DisplayAlert("Save failed", "Unable to save the current world asset.", "OK");
                    return;
                }
            }

            if (!worldService.LoadWorld(worldFile))
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
                await Application.Current.MainPage.DisplayAlert("Rename failed", "Asset with this name already exists or the name is invalid.", "OK");
            }
        }

        async Task DeleteAsset(AssetFile assetFile)
        {
            var confirmed = await Application.Current.MainPage.DisplayAlert(
                "Delete asset",
                $"Delete {assetFile.DisplayName}?",
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

        void ToggleFolder(ContentListRow row)
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
                AppendChildren(rows, foldersByParent, assetsByFolder, foldersById, -1, 1, projection);
            }

            ReplaceRows(visibleRows, rows);
            RestoreSelection();
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
                        isExpanded ? "folder_open_24.png" : "folder_24.png",
                        HasChildren(foldersByParent, assetsByFolder, folder.Id),
                        isExpanded,
                        projection.State.SelectedAssetFileId is null && projection.State.CurrentFolderId == folder.Id));

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
                    var id = AssetRowId(asset.FileId.Value);
                    rowModelsById[id] = asset;
                    rows.Add(new ContentListRow(
                        id,
                        depth,
                        asset.DisplayName,
                        GetAssetIcon(asset),
                        false,
                        false,
                        string.Equals(projection.State.SelectedAssetFileId, asset.FileId.Value, StringComparison.Ordinal)));
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
                expandLabel.SetBinding(IsVisibleProperty, nameof(ContentListRow.HasChildren));
                var expandTap = new TapGestureRecognizer();
                expandTap.Tapped += (_, _) =>
                {
                    if (expandLabel.BindingContext is ContentListRow row)
                    {
                        ToggleFolder(row);
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
                        rowModelsById.TryGetValue(row.Id, out var model) &&
                        model is AssetFile assetFile)
                    {
                        e.Data.Properties[EditorDragDrop.DragItemKey] = assetFile;
                    }
                };
                border.GestureRecognizers.Add(dragGesture);

                var dropGesture = new DropGestureRecognizer();
                dropGesture.DragOver += (_, e) => e.AcceptedOperation = DataPackageOperation.Copy;
                dropGesture.Drop += async (_, e) =>
                {
                    object target = null;
                    if (border.BindingContext is ContentListRow row && rowModelsById.TryGetValue(row.Id, out var model))
                    {
                        target = model is ProjectContentFolderItem ? null : model;
                    }

                    await HandleContentDrop(e, target);
                };
                border.GestureRecognizers.Add(dropGesture);

                var singleTap = new TapGestureRecognizer { NumberOfTapsRequired = 1 };
                singleTap.Tapped += async (_, _) =>
                {
                    if (border.BindingContext is ContentListRow row)
                    {
                        await SelectRow(row, true);
                    }
                };
                border.GestureRecognizers.Add(singleTap);

                var doubleTap = new TapGestureRecognizer { NumberOfTapsRequired = 2 };
                doubleTap.Tapped += async (_, _) =>
                {
                    if (border.BindingContext is not ContentListRow row || !rowModelsById.TryGetValue(row.Id, out var model))
                    {
                        return;
                    }

                    switch (model)
                    {
                        case WorldFile worldFile:
                            await OpenWorldWithConfirmation(worldFile);
                            break;
                        case AssetFolder or ProjectContentFolderItem:
                            ToggleFolder(row);
                            break;
                    }
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
                contextMenu.Show(
                    new EditorContextMenuItem
                    {
                        Text = "Rename",
                        Command = new Command(async () => await RenameAsset(assetFile))
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Delete",
                        IsDestructive = true,
                        Command = new Command(async () => await DeleteAsset(assetFile))
                    });
            }
        }

        void RestoreSelection()
        {
            var selected = visibleRows.FirstOrDefault(row => row.IsSelected);
            suppressSelectionChanged = true;
            try
            {
                ContentList.SelectedItem = selected;
            }
            finally
            {
                suppressSelectionChanged = false;
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
            => projection.State.SelectedAssetFileId is null && projection.State.CurrentFolderId is null;

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
            _ => "document_24.png"
        };

        static string FolderRowId(int folderId) => $"folder:{folderId}";

        static string AssetRowId(string fileId) => $"asset:{fileId}";

        static bool TryParseFolderId(string id, out int folderId)
        {
            folderId = default;
            return id.StartsWith("folder:", StringComparison.Ordinal) &&
                int.TryParse(id["folder:".Length..], out folderId);
        }

        static void ReplaceRows(ObservableCollection<ContentListRow> target, IReadOnlyList<ContentListRow> desired)
        {
            target.Clear();
            foreach (var row in desired)
            {
                target.Add(row);
            }
        }
    }

    public sealed record ContentListRow(string Id, int Depth, string Label, string Icon, bool HasChildren, bool IsExpanded, bool IsSelected)
    {
        public string ExpandGlyph => IsExpanded ? "−" : "+";
    }
}
