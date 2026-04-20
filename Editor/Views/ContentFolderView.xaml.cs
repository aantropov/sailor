using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Content;
using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;

namespace SailorEditor.Views
{
    public partial class ContentFolderView : ContentView
    {
        readonly AssetsService service;
        readonly ProjectContentStore contentStore;
        readonly ICommandDispatcher dispatcher;
        readonly IActionContextProvider contextProvider;
        bool suppressSelectionChanged;

        public ContentFolderView()
        {
            InitializeComponent();

            service = MauiProgram.GetService<AssetsService>();
            contentStore = MauiProgram.GetService<ProjectContentStore>();
            dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            contextProvider = MauiProgram.GetService<IActionContextProvider>();

            SortPicker.ItemsSource = Enum.GetValues<ProjectContentSortMode>().Cast<object>().ToList();
            SortPicker.SelectedItem = contentStore.State.SortMode;
            FilterSearchBar.Text = contentStore.State.Filter;

            PopulateTreeView(contentStore.Projection);

            contentStore.ProjectionChanged += PopulateTreeView;
            FolderTree.SelectedItemChanged += OnSelectTreeViewNode;
            FolderTree.DropRequested += OnContentDropRequested;
            FolderTree.ContextRequested += OnContentContextRequested;
            CurrentFoldersView.SelectionChanged += OnCurrentFolderSelectionChanged;
            CurrentAssetsView.SelectionChanged += OnCurrentAssetSelectionChanged;
            FilterSearchBar.TextChanged += OnFilterTextChanged;
            SortPicker.SelectedIndexChanged += OnSortChanged;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectAssetAction += SelectAssetFile;
        }

        void SelectAssetFile(ObservableObject obj)
        {
            if (obj is not AssetFile file || file.FileId is null || file.FileId.IsEmpty())
            {
                suppressSelectionChanged = true;
                try
                {
                    FolderTree.SelectedItem = null;
                    CurrentAssetsView.SelectedItem = null;
                }
                finally
                {
                    suppressSelectionChanged = false;
                }
                return;
            }

            var currentFileId = (FolderTree.SelectedItem?.BindingContext as TreeViewItem<AssetFile>)?.Model.FileId;
            if (currentFileId == file.FileId)
            {
                return;
            }

            contentStore.SelectAsset(file.FileId.Value);
        }

        void OnFilterTextChanged(object? sender, TextChangedEventArgs e)
        {
            if (string.Equals(e.NewTextValue, contentStore.State.Filter, StringComparison.Ordinal))
            {
                return;
            }

            contentStore.SetFilter(e.NewTextValue);
        }

        void OnSortChanged(object? sender, EventArgs e)
        {
            if (SortPicker.SelectedItem is not ProjectContentSortMode sortMode || sortMode == contentStore.State.SortMode)
            {
                return;
            }

            contentStore.SetSort(sortMode);
        }

        void OnCurrentFolderSelectionChanged(object? sender, SelectionChangedEventArgs e)
        {
            if (suppressSelectionChanged)
            {
                return;
            }

            if (e.CurrentSelection.FirstOrDefault() is ProjectContentFolderItem folder)
            {
                contentStore.SelectFolder(folder.FolderId);
            }

            CurrentFoldersView.SelectedItem = null;
        }

        void OnCurrentAssetSelectionChanged(object? sender, SelectionChangedEventArgs e)
        {
            if (suppressSelectionChanged)
            {
                return;
            }

            if (e.CurrentSelection.FirstOrDefault() is ProjectContentAssetItem asset)
            {
                OpenAssetById(asset.FileId);
            }

            CurrentAssetsView.SelectedItem = null;
        }

        void OpenAssetById(string fileId)
        {
            if (!service.Files.Any(x => x.FileId?.Value == fileId))
            {
                return;
            }

            var assetFile = service.Files.First(x => x.FileId?.Value == fileId);
            contentStore.SelectAsset(fileId);
            dispatcher.DispatchAsync(
                    new OpenAssetCommand(assetFile),
                    contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(ContentFolderView))))
                .GetAwaiter()
                .GetResult();
        }

        void OnSelectTreeViewNode(object sender, EventArgs args)
        {
            if (suppressSelectionChanged)
            {
                return;
            }

            var selectionChanged = args as TreeView.OnSelectItemEventArgs;
            switch (selectionChanged?.Model)
            {
                case TreeViewItem<AssetFile> assetFile:
                    contentStore.SelectAsset(assetFile.Model.FileId?.Value);
                    dispatcher.DispatchAsync(
                            new OpenAssetCommand(assetFile.Model),
                            contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(ContentFolderView))))
                        .GetAwaiter()
                        .GetResult();
                    break;
                case TreeViewItemGroup<AssetFolder, AssetFile> folder:
                    contentStore.SelectFolder(folder.Model?.Id);
                    break;
            }
        }

        async void OnContentDropRequested(object sender, TreeViewDropRequest request)
        {
            if (!EditorDragDrop.TryCreateContentDropCommand(request.Source, request.Target, out var command, out var requiresConfirmation) || command is null)
            {
                return;
            }

            if (requiresConfirmation && request.Target is PrefabFile prefab && request.Source is GameObject gameObject)
            {
                request.Handled = true;
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
            request.Handled = (await dispatcher.DispatchAsync(command, context)).Succeeded;
        }

        void OnContentContextRequested(object sender, TreeView.OnContextRequestedEventArgs args)
        {
            var contextMenu = MauiProgram.GetService<EditorContextMenuService>();
            if (args.Model is TreeViewItem<AssetFile> assetItem)
            {
                contextMenu.Show(
                    new EditorContextMenuItem
                    {
                        Text = "Rename",
                        Command = new Command(async () => await RenameAsset(assetItem.Model))
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Delete",
                        IsDestructive = true,
                        Command = new Command(async () => await DeleteAsset(assetItem.Model))
                    });
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

        void PopulateTreeView(ProjectContentProjection projection)
        {
            var expandedFolderIds = CaptureExpandedFolderIds(FolderTree.RootNodes);
            var foldersById = service.Folders.ToDictionary(x => x.Id);
            var assetsByFileId = service.Files
                .Where(x => x.FileId is not null && !x.FileId.IsEmpty())
                .ToDictionary(x => x.FileId!.Value, StringComparer.Ordinal);

            var foldersModel = FolderTreeViewBuilder.PopulateDirectory(projection, foldersById, assetsByFileId);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel, new TreeViewPopulateArgs());
            FolderTree.RootNodes = rootNodes;
            RestoreExpandedFolders(expandedFolderIds);
            RestoreSelection(projection);
            UpdateBrowserSurface(projection);
        }

        void UpdateBrowserSurface(ProjectContentProjection projection)
        {
            var currentFolder = GetCurrentFolder(projection);
            CurrentFolderTitle.Text = currentFolder.Name;
            CurrentFolderPath.Text = currentFolder.FullPath;
            CurrentFolderSummary.Text = $"{projection.CurrentFolderFolders.Count} folders • {projection.CurrentFolderAssets.Count} assets";

            if (!string.Equals(FilterSearchBar.Text, projection.State.Filter, StringComparison.Ordinal))
            {
                FilterSearchBar.Text = projection.State.Filter;
            }

            if (SortPicker.SelectedItem is not ProjectContentSortMode selectedSort || selectedSort != projection.State.SortMode)
            {
                SortPicker.SelectedItem = projection.State.SortMode;
            }

            CurrentFoldersView.ItemsSource = projection.CurrentFolderFolders;
            CurrentAssetsView.ItemsSource = projection.CurrentFolderAssets;
        }

        ProjectContentFolderItem GetCurrentFolder(ProjectContentProjection projection)
        {
            if (projection.State.CurrentFolderId is not { } folderId)
            {
                return projection.Root;
            }

            return projection.Folders.FirstOrDefault(x => x.FolderId == folderId) ?? projection.Root;
        }

        void RestoreSelection(ProjectContentProjection projection)
        {
            TreeViewNode? selectedNode = null;
            if (!string.IsNullOrWhiteSpace(projection.State.SelectedAssetFileId))
            {
                selectedNode = FindAssetNode(projection.State.SelectedAssetFileId);
            }

            selectedNode ??= projection.State.CurrentFolderId is { } folderId
                ? FindFolderNode(folderId)
                : null;

            suppressSelectionChanged = true;
            try
            {
                if (selectedNode is null)
                {
                    FolderTree.SelectedItem = null;
                    return;
                }

                ExpandAncestors(selectedNode);
                selectedNode.Select();
                FolderTree.SelectedItem = selectedNode;
            }
            finally
            {
                suppressSelectionChanged = false;
            }
        }

        TreeViewNode? FindAssetNode(string fileId)
        {
            foreach (var node in FolderTree.RootNodes)
            {
                var match = node.FindFileRecursive(new AssetFile { FileId = new FileId(fileId) });
                if (match != null)
                {
                    return match;
                }
            }

            return null;
        }

        TreeViewNode? FindFolderNode(int folderId)
        {
            foreach (var node in FolderTree.RootNodes)
            {
                var match = node.FindFolderRecursive(folderId);
                if (match != null)
                {
                    return match;
                }
            }

            return null;
        }

        static HashSet<int> CaptureExpandedFolderIds(IEnumerable<TreeViewNode> nodes)
        {
            var expandedFolderIds = new HashSet<int>();
            foreach (var node in nodes)
            {
                if (node.IsExpanded &&
                    node.BindingContext is TreeViewItemGroup<AssetFolder, AssetFile> folder &&
                    folder.Model is not null)
                {
                    expandedFolderIds.Add(folder.Model.Id);
                }

                foreach (var childFolderId in CaptureExpandedFolderIds(node.ChildrenList))
                {
                    expandedFolderIds.Add(childFolderId);
                }
            }

            return expandedFolderIds;
        }

        void RestoreExpandedFolders(HashSet<int> expandedFolderIds)
        {
            foreach (var folderId in expandedFolderIds)
            {
                FindFolderNode(folderId)?.SetExpandedSilently(true);
            }
        }

        static void ExpandAncestors(TreeViewNode node)
        {
            var parent = node.ParentTreeViewItem;
            while (parent is not null)
            {
                parent.SetExpandedSilently(true);
                parent = parent.ParentTreeViewItem;
            }
        }
    }
}
