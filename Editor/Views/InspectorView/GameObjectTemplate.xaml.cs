using SailorEditor.ViewModels;
using SailorEditor.Services;
using SailorEditor.Commands;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.Workflow;
using SailorEngine;

namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Skip)]
public partial class GameObjectTemplate : DataTemplate
{
    public GameObjectTemplate()
    {
        InitializeComponent();
    }

    void OnEntryCompleted(object sender, EventArgs e)
    {
        ((Entry)sender).Unfocus();
    }

    void OnEntryUnfocused(object sender, FocusEventArgs e)
    {
        if (sender is not Entry { BindingContext: IInspectorEditable editable } entry)
            return;

        entry.Dispatcher.DispatchDelayed(TimeSpan.FromMilliseconds(1), async () =>
        {
            if (!editable.HasPendingInspectorChanges)
                return;

            try
            {
                await editable.CommitInspectorChangesAsync();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector commit failed: {ex}");
            }
        });
    }

    async void OnAddComponentClicked(object sender, EventArgs e)
    {
        if (ResolveGameObject(sender) is GameObject gameObject)
            await gameObject.AddComponentFromInspectorAsync();
    }

    async void OnApplyPrefabClicked(object sender, EventArgs e)
    {
        if (ResolveGameObject(sender) is not GameObject gameObject)
            return;

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
                    nameof(GameObjectTemplate)));
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
        if (ResolveGameObject(sender) is not GameObject
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
                    nameof(GameObjectTemplate)));
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

    static GameObject ResolveGameObject(object sender)
    {
        if (sender is BindableObject { BindingContext: GameObject direct })
            return direct;

        if (sender is Element element)
        {
            for (var current = element.Parent; current is not null; current = current.Parent)
            {
                if (current.BindingContext is GameObject gameObject)
                    return gameObject;
            }
        }

        return MauiProgram.GetService<SelectionService>().SelectedItem as GameObject;
    }
}
