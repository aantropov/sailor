using SailorEditor.Services;

namespace Editor.Tests;

public sealed class GameObjectClipboardCodecTests
{
    [Fact]
    public void GroupClipboard_RoundTripsIndependentHierarchiesAndParents()
    {
        var entries = new[]
        {
            new GameObjectClipboardEntry("name: Объект\nchildren: []\n", "parent-a"),
            new GameObjectClipboardEntry("name: Object B\n", null)
        };

        Assert.True(GameObjectClipboardCodec.TryDecode(GameObjectClipboardCodec.Encode(entries), out var decoded));
        Assert.Equal(entries, decoded);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("ordinary text")]
    [InlineData("SailorEditor.GameObject/1\nnot json")]
    [InlineData("SailorEditor.GameObject/1\n[]")]
    [InlineData("SailorEditor.GameObject/1\n[null]")]
    [InlineData("SailorEditor.GameObject/1\n[{\"PrefabYaml\":\" \"}]")]
    public void InvalidClipboard_DoesNotProducePartialSelection(string? text)
    {
        Assert.False(GameObjectClipboardCodec.TryDecode(text, out var entries));
        Assert.Empty(entries);
    }
}
