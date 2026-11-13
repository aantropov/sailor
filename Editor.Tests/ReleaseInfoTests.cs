using SailorEditor.Services;
using System.Reflection;

namespace SailorEditor.Editor.Tests;

public sealed class ReleaseInfoTests
{
    [Fact]
    public void CompiledEditorAndEngineMetadataHaveTheSameReleaseVersion()
    {
        var assembly = typeof(ReleaseInfo).Assembly;
        Assert.Equal(assembly.GetName().Version!.ToString(3), ReleaseInfo.EditorVersion);
        Assert.Equal(ReleaseInfo.EditorVersion, ReleaseInfo.EngineVersion);
        Assert.Contains(ReleaseInfo.EditorVersion, ReleaseInfo.DisplayText);
        Assert.Equal("https://github.com/aantropov/sailor/releases", ReleaseInfo.ReleasesUrl);
    }
}
