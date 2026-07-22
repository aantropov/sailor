namespace Editor.Tests;

public sealed class AssetInfoSerializationSourceContractTests
{
    [Theory]
    [InlineData("Sailor::AssetInfo")]
    [InlineData("Sailor::AnimationAssetInfo")]
    [InlineData("Sailor::FrameGraphAssetInfo")]
    [InlineData("Sailor::MaterialAssetInfo")]
    [InlineData("Sailor::ModelAssetInfo")]
    [InlineData("Sailor::PrefabAssetInfo")]
    [InlineData("Sailor::ShaderAssetInfo")]
    [InlineData("Sailor::TextureAssetInfo")]
    [InlineData("Sailor::WorldPrefabAssetInfo")]
    public void EditorAndRepositoryContractsCoverEveryKnownAssetInfoType(string assetInfoType)
    {
        var editorSource = ReadRepositoryFile("Editor", "ViewModels", "AssetFile.cs");
        var repositoryContract = ReadRepositoryFile("Editor.Tests", "RepositoryAssetSidecarContractTests.cs");

        Assert.Contains($"\"{assetInfoType}\"", editorSource, StringComparison.Ordinal);
        Assert.Contains($"\"{assetInfoType}\"", repositoryContract, StringComparison.Ordinal);
    }

    [Fact]
    public void EveryEditorMetadataSaveInjectsTypeAndFiltersRuntimeOnlyFields()
    {
        var source = ReadRepositoryFile("Editor", "ViewModels", "AssetFile.cs");

        Assert.Contains("{ \"assetInfoType\", ResolveAssetInfoTypeNameForSave() }", source, StringComparison.Ordinal);
        Assert.Contains("WriteAssetInfoYaml(BuildAssetInfoYamlNode())", source, StringComparison.Ordinal);
        Assert.Contains("CopyPortableAssetInfoFields(originalRoot, mergedRoot)", source, StringComparison.Ordinal);
        Assert.Contains("CopyPortableAssetInfoFields(generatedRoot, mergedRoot)", source, StringComparison.Ordinal);
        Assert.Contains("RuntimeOnlyAssetInfoFields.Contains(fieldName)", source, StringComparison.Ordinal);
        Assert.Contains("RuntimeOnlyAssetInfoFields.Contains(field.Key)", source, StringComparison.Ordinal);
    }

    [Fact]
    public void CustomWorkspaceAssetInfoTypePrecedesBuiltInFallbackInference()
    {
        var source = ReadRepositoryFile("Editor", "ViewModels", "AssetFile.cs");
        var customType = source.IndexOf("NormalizeAssetInfoType(AssetInfoTypeName)", StringComparison.Ordinal);
        var runtimeCatalogType = source.IndexOf("NormalizeAssetInfoType(AssetType?.Name)", StringComparison.Ordinal);
        var builtInFallback = source.IndexOf("?? inferredType", StringComparison.Ordinal);

        Assert.True(customType >= 0);
        Assert.True(runtimeCatalogType > customType);
        Assert.True(builtInFallback > runtimeCatalogType);
    }

    [Theory]
    [InlineData("AssetFile.cs")]
    [InlineData("TextureFile.cs")]
    [InlineData("ModelFile.cs")]
    public void LegacyYamlConvertersRoundTripAssetInfoType(string filename)
    {
        var source = ReadRepositoryFile("Editor", "ViewModels", filename);

        Assert.Contains("case \"assetInfoType\"", source, StringComparison.Ordinal);
        Assert.Contains("new Scalar(null, \"assetInfoType\")", source, StringComparison.Ordinal);
        Assert.Contains("ResolveAssetInfoTypeNameForSave()", source, StringComparison.Ordinal);
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                Directory.Exists(Path.Combine(current.FullName, "Runtime")))
            {
                return File.ReadAllText(Path.Combine([current.FullName, .. relativePath]));
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
