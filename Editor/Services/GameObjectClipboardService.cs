using SailorEditor.Commands;
using SailorEditor.Shell;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;

namespace SailorEditor.Services;

internal sealed class GameObjectClipboardService
{
    string? copiedPayload;

    public Task CopyAsync(GameObject gameObject, CancellationToken cancellationToken = default)
        => CopyAsync(new[] { gameObject }, cancellationToken);

    public async Task CopyAsync(IEnumerable<GameObject> gameObjects, CancellationToken cancellationToken = default)
    {
        var roots = GetRoots(gameObjects);
        cancellationToken.ThrowIfCancellationRequested();
        var world = MauiProgram.GetService<WorldService>();
        copiedPayload = GameObjectClipboardCodec.Encode(roots.Select(gameObject =>
        {
            var prefab = world.CreatePrefabFromSubHierarchy(gameObject, out _);
            return new GameObjectClipboardEntry(
                EditorYaml.SerializePrefab(prefab),
                world.ResolveParentInstanceId(gameObject)?.Value);
        }));

        try
        {
            await Clipboard.Default.SetTextAsync(copiedPayload);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"Could not publish GameObjects to the system clipboard: {exception}");
        }

        MauiProgram.GetService<EditorShellHost>().SetStatus(
            roots.Count == 1 ? $"Copied {roots[0].Name}" : $"Copied {roots.Count} GameObjects");
    }

    public async Task PasteAsync(CancellationToken cancellationToken = default)
    {
        string? systemClipboardText = null;
        try
        {
            systemClipboardText = await Clipboard.Default.GetTextAsync();
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"Could not read the system clipboard: {exception}");
        }

        cancellationToken.ThrowIfCancellationRequested();
        if (!GameObjectClipboardCodec.TryDecode(systemClipboardText, out var entries) &&
            !GameObjectClipboardCodec.TryDecode(copiedPayload, out entries))
        {
            throw new InvalidDataException("The clipboard does not contain Sailor GameObjects.");
        }

        var world = MauiProgram.GetService<WorldService>();
        var commands = entries.Select(entry =>
        {
            InstanceId? parentId = null;
            if (!string.IsNullOrWhiteSpace(entry.ParentInstanceId))
            {
                var candidate = new InstanceId(entry.ParentInstanceId);
                if (world.TryGetGameObject(candidate, out _))
                {
                    parentId = candidate;
                }
            }

            return new DuplicateGameObjectCommand(entry.PrefabYaml, parentId);
        });

        await DispatchAsync(EditorCommandBatch.Create("Paste GameObjects", commands), cancellationToken);
        MauiProgram.GetService<EditorShellHost>().SetStatus(
            entries.Count == 1 ? "Pasted GameObject" : $"Pasted {entries.Count} GameObjects");
    }

    public Task DuplicateAsync(GameObject gameObject, CancellationToken cancellationToken = default)
        => DuplicateAsync(new[] { gameObject }, cancellationToken);

    public async Task DuplicateAsync(IEnumerable<GameObject> gameObjects, CancellationToken cancellationToken = default)
    {
        var roots = GetRoots(gameObjects);
        await DispatchAsync(
            EditorCommandBatch.Create("Duplicate GameObjects", roots.Select(gameObject => new DuplicateGameObjectCommand(gameObject))),
            cancellationToken);
        MauiProgram.GetService<EditorShellHost>().SetStatus(
            roots.Count == 1 ? $"Duplicated {roots[0].Name}" : $"Duplicated {roots.Count} GameObjects");
    }

    static IReadOnlyList<GameObject> GetRoots(IEnumerable<GameObject> gameObjects)
    {
        ArgumentNullException.ThrowIfNull(gameObjects);
        var roots = EditorDragDrop.GetSelectionRoots(gameObjects).OfType<GameObject>().ToArray();
        if (roots.Length == 0)
        {
            throw new ArgumentException("Select at least one GameObject.", nameof(gameObjects));
        }

        return roots;
    }

    static async Task DispatchAsync(IEditorCommand command, CancellationToken cancellationToken)
    {
        var result = await MauiProgram.GetService<ICommandDispatcher>().DispatchAsync(
            command,
            MauiProgram.GetService<IActionContextProvider>().GetCurrentContext(
                new CommandOrigin(CommandOriginKind.Menu, nameof(GameObjectClipboardService))),
            cancellationToken);
        if (!result.Succeeded)
        {
            throw new InvalidOperationException(result.Message ?? "GameObject clipboard operation failed.");
        }

        var createdIds = EditorCommandBatch.ResultValues<InstanceId>(result).ToArray();
        if (createdIds.Length > 0)
        {
            MauiProgram.GetService<SelectionService>().SelectInstances(createdIds);
        }
    }
}
