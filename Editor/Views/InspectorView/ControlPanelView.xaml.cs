using Microsoft.Maui.Controls;
using Microsoft.Maui.Controls.Xaml;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using System;
using System.ComponentModel;
using SailorEditor.Commands;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.Workflow;
using SailorEngine;
using SailorEditor.Utility;

namespace SailorEditor.Views;

[XamlCompilation(XamlCompilationOptions.Compile)]
public partial class ControlPanelView : ContentView
{
    INotifyPropertyChanged? observedItem;

    public ControlPanelView()
    {
        InitializeComponent();
        BindingContextChanged += (_, _) => BindHeader();
        Loaded += (_, _) => BindHeader();
        Unloaded += (_, _) => Unsubscribe();
    }

    void Unsubscribe()
    {
        if (observedItem is not null)
        {
            observedItem.PropertyChanged -= OnItemChanged;
            observedItem = null;
        }
    }

    void BindHeader()
    {
        Unsubscribe();
        observedItem = BindingContext as INotifyPropertyChanged;
        if (observedItem is not null)
        {
            observedItem.PropertyChanged += OnItemChanged;
        }
        RefreshHeader();
    }

    void OnItemChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (args.PropertyName is nameof(AssetFile.DisplayName) or nameof(AssetFile.IsDirty) or
            nameof(AssetFile.IsReadOnly) or nameof(GameObject.IsPrefabLinked))
        {
            Dispatcher.Dispatch(RefreshHeader);
        }
    }

    void RefreshHeader()
    {
        TitleLabel.Text = BindingContext switch
        {
            AssetFile asset => $"{(asset.IsDirty ? "*" : string.Empty)}{asset.DisplayName} (Type: {asset.GetType().Name})",
            GameObject gameObject => gameObject.DisplayName,
            ViewModels.Component component => component.Typename?.Name ?? "Component",
            AggregateInspectorModel aggregate => $"{(aggregate.IsDirty ? "*" : string.Empty)}{aggregate.DisplayName}",
            _ => string.Empty
        };
        AssetActions.IsVisible = BindingContext is AssetFile;
        AssetActions.IsEnabled = BindingContext is not AssetFile { IsReadOnly: true };
        PrefabActions.IsVisible = BindingContext is GameObject { IsPrefabLinked: true };
        AggregateAssetActions.IsVisible = BindingContext is AggregateInspectorModel { IsAssetSelection: true, IsDirty: true };
    }

    async void OnSaveSelectionClicked(object sender, EventArgs args)
    {
        if (BindingContext is not AggregateInspectorModel aggregate)
        {
            return;
        }
        try
        {
            if (!await aggregate.SaveAsync())
            {
                MauiProgram.GetService<EngineService>().PushConsoleMessage("Some selected assets could not be saved or updated in the engine.");
            }
            MauiProgram.GetService<InspectorProjectionService>().Refresh();
        }
        catch (Exception error)
        {
            MauiProgram.GetService<EngineService>().PushConsoleMessage($"Save selection failed: {error.Message}");
        }
    }

    async void OnRevertSelectionClicked(object sender, EventArgs args)
    {
        if (BindingContext is not AggregateInspectorModel aggregate)
        {
            return;
        }
        try
        {
            await aggregate.RevertAsync();
            MauiProgram.GetService<InspectorProjectionService>().Refresh();
        }
        catch (Exception error)
        {
            MauiProgram.GetService<EngineService>().PushConsoleMessage($"Revert selection failed: {error.Message}");
        }
    }

    private void OnOpenButtonClicked(object sender, EventArgs e)
    {
        var assetFile = (sender as Button)?.BindingContext as AssetFile;
        assetFile?.Open();
    }

    private async void OnSaveButtonClicked(object sender, EventArgs e)
    {
        if (sender is Button { BindingContext: AssetFile assetFile })
        {
            var assetsService = MauiProgram.GetService<AssetsService>();
            if (!await assetsService.SaveAssetAsync(assetFile))
            {
                MauiProgram.GetService<EngineService>().PushConsoleMessage(
                    $"Asset was saved, but its targeted native update did not commit: {assetFile.AssetInfo.FullName}");
            }
        }
    }

    private async void OnRevertButtonClicked(object sender, EventArgs e)
    {
        if (sender is Button { BindingContext: AssetFile assetFile })
        {
            await assetFile.Revert();
        }
    }

    async void OnApplyPrefabClicked(object sender, EventArgs e)
    {
        if ((sender as BindableObject)?.BindingContext is not GameObject gameObject)
        {
            return;
        }

        var host = MauiProgram.GetService<EditorShellHost>();
        try
        {
            if (!await MauiProgram
                    .GetService<InspectorPendingEditCoordinator>()
                    .CommitPendingChangesAsync())
            {
                throw new InvalidOperationException(
                    "Pending Inspector changes could not be committed.");
            }

            var context = MauiProgram
                .GetService<IActionContextProvider>()
                .GetCurrentContext(new CommandOrigin(
                    CommandOriginKind.UI,
                    nameof(ControlPanelView)));
            var result = await MauiProgram
                .GetService<ICommandDispatcher>()
                .DispatchAsync(
                    new ApplyPrefabInstanceCommand(gameObject),
                    context);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(
                    result.Message ?? "Apply prefab failed.");
            }
            host.SetStatus("Prefab changes applied.");
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Apply prefab failed: {exception}");
            host.SetStatus($"Apply prefab failed: {exception.Message}");
        }
    }

    async void OnSelectPrefabClicked(object sender, EventArgs e)
    {
        if ((sender as BindableObject)?.BindingContext is not GameObject
            {
                PrefabFileId: not null
            } gameObject ||
            string.IsNullOrWhiteSpace(gameObject.PrefabFileId))
        {
            return;
        }

        var host = MauiProgram.GetService<EditorShellHost>();
        try
        {
            var fileId = new FileId(gameObject.PrefabFileId);
            if (!MauiProgram.GetService<AssetsService>()
                    .Assets.TryGetValue(fileId, out var asset) ||
                asset is not PrefabFile prefab)
            {
                throw new InvalidOperationException(
                    $"Prefab asset '{fileId.Value}' is not available.");
            }

            await host.OpenPanelAsync(KnownPanelTypes.Content);
            var context = MauiProgram
                .GetService<IActionContextProvider>()
                .GetCurrentContext(new CommandOrigin(
                    CommandOriginKind.UI,
                    nameof(ControlPanelView)));
            var result = await MauiProgram
                .GetService<ICommandDispatcher>()
                .DispatchAsync(new OpenAssetCommand(prefab), context);
            if (!result.Succeeded)
            {
                throw new InvalidOperationException(
                    result.Message ?? "Select prefab failed.");
            }
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Select prefab failed: {exception}");
            host.SetStatus($"Select prefab failed: {exception.Message}");
        }
    }
}
