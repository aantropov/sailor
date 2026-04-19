using SailorEditor.Controls;
using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using SailorEditor.Services;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEditor.Content;
using SailorEditor.Commands;

namespace SailorEditor.Views
{
    public partial class ContentFolderView : ContentView
    {
        readonly AssetsService service;
        readonly ProjectContentStore contentStore;
        readonly ICommandDispatcher dispatcher;
        readonly IActionContextProvider contextProvider;
        public ContentFolderView()
        {
            InitializeComponent();

            service = MauiProgram.GetService<AssetsService>();
            contentStore = MauiProgram.GetService<ProjectContentStore>();
            dispatcher = MauiProgram.GetService<ICommandDispatcher>();
            contextProvider = MauiProgram.GetService<IActionContextProvider>();

            PopulateTreeView();

            FolderTree.SelectedItemChanged += OnSelectTreeViewNode;
            FolderTree.DropRequested += OnContentDropRequested;
            FolderTree.ContextRequested += OnContentContextRequested;

            var selectionViewModel = MauiProgram.GetService<SelectionService>();
            selectionViewModel.OnSelectAssetAction += SelectAssetFile;
        }

        private void SelectAssetFile(ObservableObject obj)
        {
            if (obj is AssetFile file)
            {
                if (FolderTree.SelectedItem != null)
                {
                    var assetFile = FolderTree.SelectedItem.BindingContext as TreeViewItem<AssetFile>;

                    if (assetFile.Model.FileId == file.FileId)
                    {
                        return;
                    }
                }

                foreach (var el in FolderTree.RootNodes)
                {
                    var res = el.FindFileRecursive(file);

                    if (res != null)
                    {
                        res.Select();
                        FolderTree.SelectedItem = res;
                        break;
                    }
                }
            }
        }

        public static void OnSelectTreeViewNode(object sender, EventArgs args)
        {
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

        private async void OnContentDropRequested(object sender, TreeViewDropRequest request)
        {
            if (request.Source is not GameObject gameObject)
            {
                return;
            }

            var context = contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.DragDrop, nameof(ContentFolderView)));
            switch (request.Target)
            {
                case null:
                    request.Handled = (await dispatcher.DispatchAsync(new CreatePrefabAssetCommand(gameObject), context)).Succeeded;
                    break;

                case AssetFolder folder:
                    request.Handled = (await dispatcher.DispatchAsync(new CreatePrefabAssetCommand(gameObject, folder), context)).Succeeded;
                    break;

                case PrefabFile prefab:
                    request.Handled = true;
                    var overwrite = await Application.Current.MainPage.DisplayAlert(
                        "Overwrite prefab",
                        $"Overwrite {prefab.DisplayName} with {gameObject.DisplayName}?",
                        "Overwrite",
                        "Cancel");

                    if (overwrite)
                    {
                        request.Handled = (await dispatcher.DispatchAsync(new CreatePrefabAssetCommand(gameObject, existingPrefab: prefab), context)).Succeeded;
                    }
                    break;
            }

            if (request.Handled)
                PopulateTreeView();
        }

        private void OnContentContextRequested(object sender, TreeView.OnContextRequestedEventArgs args)
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

        private async Task RenameAsset(AssetFile assetFile)
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

            if (result.Succeeded)
            {
                PopulateTreeView();
            }
            else
            {
                await Application.Current.MainPage.DisplayAlert("Rename failed", "Asset with this name already exists or the name is invalid.", "OK");
            }
        }

        private async Task DeleteAsset(AssetFile assetFile)
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

            if (result.Succeeded)
                PopulateTreeView();
            else
                await Application.Current.MainPage.DisplayAlert("Delete failed", result.Message ?? "Unable to delete asset.", "OK");
        }

        private void PopulateTreeView()
        {
            contentStore.Refresh();
            var foldersModel = FolderTreeViewBuilder.PopulateDirectory(service);
            var rootNodes = Controls.TreeView.PopulateGroup(foldersModel, new TreeViewPopulateArgs());
            FolderTree.RootNodes = rootNodes;
        }
    }
}
