using System.Text.Json;

namespace SailorEditor.Services;

internal sealed record GameObjectClipboardEntry(string PrefabYaml, string? ParentInstanceId);

internal static class GameObjectClipboardCodec
{
    const string Header = "SailorEditor.GameObject/1\n";

    public static string Encode(IEnumerable<GameObjectClipboardEntry> entries)
    {
        ArgumentNullException.ThrowIfNull(entries);
        var snapshot = entries.ToArray();
        if (!IsValid(snapshot))
        {
            throw new ArgumentException("The clipboard must contain at least one serialized hierarchy.", nameof(entries));
        }

        return Header + JsonSerializer.Serialize(snapshot);
    }

    public static bool TryDecode(string? text, out IReadOnlyList<GameObjectClipboardEntry> entries)
    {
        entries = Array.Empty<GameObjectClipboardEntry>();
        if (text is null || !text.StartsWith(Header, StringComparison.Ordinal))
        {
            return false;
        }

        try
        {
            var decoded = JsonSerializer.Deserialize<GameObjectClipboardEntry[]>(text[Header.Length..]);
            if (decoded is null || !IsValid(decoded))
            {
                return false;
            }

            entries = Array.AsReadOnly(decoded);
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    static bool IsValid(IReadOnlyList<GameObjectClipboardEntry> entries)
        => entries.Count > 0 && entries.All(entry => entry is not null && !string.IsNullOrWhiteSpace(entry.PrefabYaml));
}
