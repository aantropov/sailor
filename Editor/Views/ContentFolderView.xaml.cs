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

            PopulateTreeView(contentStore.Projection);

            contentStore.ProjectionChanged += PopulateTreeView;
            FolderTree.SelectedItemChanged += OnSelectTreeViewNode;
            FolderTree.DropRequested += OnContentDropRequested;
            FolderTree.ContextRequested += OnContentContextRequested;

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
                    MauiProgram.GetService<ProjectContentStore>().SelectAsset(assetFile.Model.FileId?.Value);
                    MauiProgram.GetService<ICommandDispatcher>()
                        .DispatchAsync(
                            new OpenAssetCommand(assetFile.Model),
                            MauiProgram.GetService<IActionContextProvider>().GetCurrentContext(new CommandOrigin(CommandOriginKind.Panel, nameof(ContentFolderView))))
                        .GetAwaiter()
                        .GetResult();
                    break;
                case TreeViewItemGroup<AssetFolder, AssetFile> folder:
                    MauiProgram.GetService<ProjectContentStore>().SelectFolder(folder.Model?.Id);
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
            var foldersById = service.Folders.ToDictionary(x => x.Id);
            var assetsByFileId = service.Files
                .Where(x => x.FileId is not null && !x.FileId.IsEmpty())
                .ToDictionary(x => x.FileId!.Value, StringComparer.Ordinal);

            var foldersModel = FolderTreeViewBuilder.PopulateDirectory(projection, foldersById, assetsByFileId);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel, new TreeViewPopulateArgs());
            FolderTree.RootNodes = rootNodes;
            RestoreSelection(projection);
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
    }
}
