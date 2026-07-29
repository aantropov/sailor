#nullable enable
using SailorEngine;

namespace SailorEditor.Scene;

public static class SceneViewportAssetDropPayload
{
    public const string Prefix = "SailorEditor.Asset:";
    public const int UnbracedFileIdLength = 36;
    public const int BracedFileIdLength = 38;
    public const int MaxLength = 57;

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
