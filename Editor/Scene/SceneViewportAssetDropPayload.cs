#nullable enable
using SailorEngine;

namespace SailorEditor.Scene;

public static class SceneViewportAssetDropPayload
{
    static readonly object SelectionLock = new();
    static readonly Dictionary<string, (FileId[] FileIds, long WorkspaceEpoch, DateTimeOffset CreatedAt)> Selections = new(StringComparer.OrdinalIgnoreCase);
    const string SelectionTokenPrefix = "ffffffff-";
    const int MaxSelections = 32;
    static readonly TimeSpan SelectionLifetime = TimeSpan.FromMinutes(5);
    public const string Prefix = "SailorEditor.Asset:";
    public const int UnbracedFileIdLength = 36;
    public const int BracedFileIdLength = 38;
    public const int MaxLength = 57;

    public static bool TryCreate(IEnumerable<FileId?> fileIds, long workspaceEpoch, out string payload)
    {
        ArgumentNullException.ThrowIfNull(fileIds);
        payload = string.Empty;
        var selection = fileIds.ToArray();
        if (selection.Length == 0 || selection.Any(fileId => fileId is null || !IsSerializedFileId(fileId.Value)))
        {
            return false;
        }

        var distinct = selection.DistinctBy(fileId => fileId!.Value, StringComparer.Ordinal).Cast<FileId>().ToArray();
        if (distinct.Length == 1)
        {
            return TryCreate(distinct[0], out payload);
        }

        // The native viewport accepts a single UUID. Keep the complete selection
        // in the Editor and send an opaque UUID through the existing drop route.
        var token = SelectionTokenPrefix + Guid.NewGuid().ToString("D")[SelectionTokenPrefix.Length..];
        lock (SelectionLock)
        {
            RemoveExpiredSelections();
            while (Selections.Count >= MaxSelections)
            {
                var oldest = Selections.MinBy(entry => entry.Value.CreatedAt).Key;
                Selections.Remove(oldest);
            }

            Selections.Add(token, (distinct.Select(fileId => new FileId(fileId.Value)).ToArray(), workspaceEpoch, DateTimeOffset.UtcNow));
        }

        return TryCreate(new FileId(token), out payload);
    }

    public static bool TryResolveFileIds(string serializedFileId, long workspaceEpoch, out IReadOnlyList<FileId> fileIds)
    {
        fileIds = Array.Empty<FileId>();
        if (!IsSerializedFileId(serializedFileId))
        {
            return false;
        }

        var canonicalId = new FileId(serializedFileId).Value;
        lock (SelectionLock)
        {
            RemoveExpiredSelections();
            if (Selections.TryGetValue(canonicalId, out var selection))
            {
                if (selection.WorkspaceEpoch != workspaceEpoch)
                {
                    return false;
                }

                fileIds = Array.AsReadOnly(selection.FileIds.Select(fileId => new FileId(fileId.Value)).ToArray());
                return true;
            }
        }

        if (canonicalId.StartsWith(SelectionTokenPrefix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        fileIds = new[] { new FileId(canonicalId) };
        return true;
    }

    public static void ResetSelections()
    {
        lock (SelectionLock)
        {
            Selections.Clear();
        }
    }

    static void RemoveExpiredSelections()
    {
        var expiry = DateTimeOffset.UtcNow - SelectionLifetime;
        foreach (var token in Selections.Where(entry => entry.Value.CreatedAt < expiry).Select(entry => entry.Key).ToArray())
        {
            Selections.Remove(token);
        }
    }

    public static bool TryCreate(
        FileId? fileId,
        out string payload)
    {
        payload = string.Empty;
        if (fileId is null ||
            !IsSerializedFileId(fileId.Value))
        {
            return false;
        }

        payload = Prefix + fileId.Value;
        return payload.Length <= MaxLength;
    }

    public static bool IsSerializedFileId(string? value)
    {
        if (value is null)
        {
            return false;
        }

        ReadOnlySpan<char> uuid;
        if (value.Length == BracedFileIdLength)
        {
            if (value[0] != '{' || value[^1] != '}')
            {
                return false;
            }

            uuid = value.AsSpan(1, UnbracedFileIdLength);
        }
        else if (value.Length == UnbracedFileIdLength)
        {
            uuid = value.AsSpan();
        }
        else
        {
            return false;
        }

        for (var index = 0; index < uuid.Length; index++)
        {
            var isHyphenPosition =
                index is 8 or 13 or 18 or 23;
            if (isHyphenPosition)
            {
                if (uuid[index] != '-')
                {
                    return false;
                }

                continue;
            }

            if (!IsHexDigit(uuid[index]))
            {
                return false;
            }
        }

        return true;
    }

    static bool IsHexDigit(char value) =>
        value is >= '0' and <= '9' or
            >= 'a' and <= 'f' or
            >= 'A' and <= 'F';
}
