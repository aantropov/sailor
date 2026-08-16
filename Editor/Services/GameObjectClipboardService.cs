using SailorEditor.Commands;
using SailorEditor.History;
using SailorEditor.Shell;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Text;

namespace SailorEditor.Services;

internal sealed record GameObjectClipboardPayload(
    string PrefabYaml,
    string? ParentInstanceId);

internal static class GameObjectClipboardCodec
{
    const string Header = "SailorEditor.GameObject/1";

    public static string Encode(GameObjectClipboardPayload payload)
    {
        ArgumentNullException.ThrowIfNull(payload);
        var parent = payload.ParentInstanceId ?? string.Empty;
        var yaml = Convert.ToBase64String(
            Encoding.UTF8.GetBytes(payload.PrefabYaml));
        return $"{Header}\n{parent}\n{yaml}";
    }

    public static bool TryDecode(
        string? text,
        out GameObjectClipboardPayload? payload)
    {
        payload = null;
        if (string.IsNullOrWhiteSpace(text))
        {
            return false;
        }

        var parts = text.Split('\n', 3);
        if (parts.Length != 3 ||
            !string.Equals(parts[0], Header, StringComparison.Ordinal))
        {
            return false;
        }

        try
        {
            var prefabYaml = Encoding.UTF8.GetString(
                Convert.FromBase64String(parts[2]));
            if (string.IsNullOrWhiteSpace(prefabYaml))
            {
                return false;
            }

            payload = new GameObjectClipboardPayload(
                prefabYaml,
                string.IsNullOrWhiteSpace(parts[1]) ? null : parts[1]);
            return true;
        }
        catch (FormatException)
        {
            return false;
        }
    }
}

internal sealed class GameObjectClipboardService
{
    string? copiedPayload;

    public async Task CopyAsync(
        GameObject gameObject,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(gameObject);
        cancellationToken.ThrowIfCancellationRequested();

        var world = MauiProgram.GetService<WorldService>();
        var prefab = world.CreatePrefabFromSubHierarchy(
            gameObject,
            out _);
        copiedPayload = GameObjectClipboardCodec.Encode(
            new GameObjectClipboardPayload(
                EditorYaml.SerializePrefab(prefab),
                world.ResolveParentInstanceId(gameObject)?.Value));

        try
        {
            await Clipboard.Default.SetTextAsync(copiedPayload);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Could not publish GameObject to the system clipboard: {exception}");
        }

        MauiProgram.GetService<EditorShellHost>()
            .SetStatus($"Copied {gameObject.Name}");
    }

    public async Task PasteAsync(
        CancellationToken cancellationToken = default)
    {
        string? systemClipboardText = null;
        try
        {
            systemClipboardText = await Clipboard.Default.GetTextAsync();
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Could not read the system clipboard: {exception}");
        }

        cancellationToken.ThrowIfCancellationRequested();
        var decoded = GameObjectClipboardCodec.TryDecode(
            systemClipboardText,
            out var payload);
        if (!decoded)
        {
            decoded = GameObjectClipboardCodec.TryDecode(
                copiedPayload,
                out payload);
        }

        if (!decoded || payload is null)
        {
            throw new InvalidDataException(
                "The clipboard does not contain a Sailor GameObject.");
        }

        var world = MauiProgram.GetService<WorldService>();
        InstanceId? parentId = null;
        if (!string.IsNullOrWhiteSpace(payload.ParentInstanceId))
        {
            var candidate = new InstanceId(payload.ParentInstanceId);
            if (world.TryGetGameObject(candidate, out _))
            {
                parentId = candidate;
            }
        }

        var result = await MauiProgram.GetService<ICommandDispatcher>()
            .DispatchAsync(
                new DuplicateGameObjectCommand(
                    payload.PrefabYaml,
                    parentId),
                MauiProgram.GetService<IActionContextProvider>()
                    .GetCurrentContext(
                        new CommandOrigin(
                            CommandOriginKind.Menu,
                            nameof(GameObjectClipboardService))),
                cancellationToken);
        if (!result.Succeeded)
        {
            throw new InvalidOperationException(
                result.Message ?? "Paste GameObject failed.");
        }

        MauiProgram.GetService<EditorShellHost>()
            .SetStatus("Pasted GameObject");
    }

    public async Task DuplicateAsync(
        GameObject gameObject,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(gameObject);
        var result = await MauiProgram.GetService<ICommandDispatcher>()
            .DispatchAsync(
                new DuplicateGameObjectCommand(gameObject),
                MauiProgram.GetService<IActionContextProvider>()
                    .GetCurrentContext(
                        new CommandOrigin(
                            CommandOriginKind.Menu,
                            nameof(GameObjectClipboardService))),
                cancellationToken);
        if (!result.Succeeded)
        {
            throw new InvalidOperationException(
                result.Message ?? "Duplicate GameObject failed.");
        }

        MauiProgram.GetService<EditorShellHost>()
            .SetStatus($"Duplicated {gameObject.Name}");
    }
}
