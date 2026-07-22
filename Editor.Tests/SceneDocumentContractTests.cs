using SailorEditor.Services;
using SailorEditor.Content;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.Editor.Tests;

public sealed class SceneDocumentContractTests
{
    [Fact]
    public void ResolveSaveTarget_AddsWorldExtensionInsideActiveContentRoot()
    {
        var root = Path.Combine(Path.GetTempPath(), "SailorSceneContract", "Content");

        var resolved = SceneDocumentContract.TryResolveSaveTarget(
            root,
            Path.Combine("Scenes", "Night"),
            out var target,
            out var error);

        Assert.True(resolved, error);
        Assert.NotNull(target);
        Assert.Equal(
            Path.Combine(ProjectContentPathPolicy.NormalizeRoot(root), "Scenes", "Night.world"),
            target.ScenePath);
        Assert.Equal(target.ScenePath + ".asset", target.AssetInfoPath);
        Assert.Equal("Night.world", target.Filename);
    }

    [Theory]
    [InlineData("../Outside.world")]
    [InlineData("Scene.prefab")]
    public void ResolveSaveTarget_RejectsEscapeAndNonWorldFiles(string requestedPath)
    {
        var root = Path.Combine(Path.GetTempPath(), "SailorSceneContract", "Content");

        Assert.False(SceneDocumentContract.TryResolveSaveTarget(
            root,
            requestedPath,
            out var target,
            out var error));
        Assert.Null(target);
        Assert.False(string.IsNullOrWhiteSpace(error));
    }

    [Fact]
    public void AssetInfo_ContainsOnlyPortableSourceMetadata()
    {
        var text = SceneDocumentContract.BuildAssetInfo("{SCENE-ID}", "Night.world");
        var yaml = new YamlStream();
        yaml.Load(new StringReader(text));
        var root = Assert.IsType<YamlMappingNode>(yaml.Documents[0].RootNode);

        Assert.Equal(
            ["assetInfoType", "fileId", "filename"],
            root.Children.Keys.Cast<YamlScalarNode>().Select(x => x.Value!).Order().ToArray());
        Assert.Equal(SceneDocumentContract.WorldAssetInfoType, root["assetInfoType"].ToString());
        Assert.Equal("{SCENE-ID}", root["fileId"].ToString());
        Assert.Equal("Night.world", root["filename"].ToString());
        Assert.DoesNotContain("load", text, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("importTime", text, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("metadataPath", text, StringComparison.OrdinalIgnoreCase);
    }
}
