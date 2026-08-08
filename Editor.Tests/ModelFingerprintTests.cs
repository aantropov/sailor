namespace Editor.Tests;

public sealed class ModelFingerprintTests
{
    [Fact]
    public void Inspector_LoadsFingerprintDirectlyFromActiveCache()
    {
        var viewModel = ReadRepositoryFile(
            "Editor",
            "ViewModels",
            "ModelFile.cs");
        var template = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView",
            "ModelFileTemplate.xaml");

        Assert.Contains("\"Fingerprints\"", viewModel);
        Assert.Contains("File.Exists(path)", viewModel);
        Assert.Contains("ImageSource.FromFile(path)", viewModel);
        Assert.Contains(
            "Path.GetFileName(fingerprintFilename)",
            viewModel);
        Assert.Contains("IsLoaded = true;", viewModel);
        Assert.Contains(
            "public override async Task Revert()",
            viewModel);
        Assert.Contains(
            "await LoadDependentResources();",
            viewModel);
        Assert.DoesNotContain("WaitForChangeAsync", viewModel);
        Assert.DoesNotContain("CancellationTokenSource", viewModel);
        Assert.Contains(
            "Source=\"{Binding Fingerprint}\"",
            template);
        Assert.Contains(
            "Binding=\"{Binding HasFingerprint}\"",
            template);
    }

    [Fact]
    public void AssetReferences_UseSharedAsyncFingerprintPreviews()
    {
        var service = ReadRepositoryFile(
            "Editor",
            "Services",
            "AssetFingerprintService.cs");
        var preview = ReadRepositoryFile(
            "Editor",
            "Controls",
            "AssetPreviewImage.cs");
        var templates = ReadRepositoryFile(
            "Editor",
            "Utility",
            "Templates.cs");
        var texture = ReadRepositoryFile(
            "Editor",
            "ViewModels",
            "TextureFile.cs");
        var converter = ReadRepositoryFile(
            "Editor",
            "Controls",
            "FileIdToPreviewTextureConverter.cs");
        var fileIdEditor = ReadRepositoryFile(
            "Editor",
            "Views",
            "Elements",
            "FileIdEditor.xaml");
        var materialTemplate = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView",
            "MaterialFileTemplate.xaml");

        Assert.Contains("\"Fingerprints\"", service, StringComparison.Ordinal);
        Assert.Contains("GlbExtractor.ExtractTextureFromGLB(", service, StringComparison.Ordinal);
        Assert.Contains("GenerateGlbFingerprint(", service, StringComparison.Ordinal);
        Assert.Contains("await Task.Run(", service, StringComparison.Ordinal);
        Assert.Contains("File.Move(temporaryPath, fingerprintPath, true)", service, StringComparison.Ordinal);
        Assert.Contains("File.GetLastWriteTimeUtc(fingerprintPath)", service, StringComparison.Ordinal);
        Assert.Contains("LoadTexturePreviewAsync(this, cancellationToken)", texture, StringComparison.Ordinal);
        Assert.Contains("class AssetPreviewImage", preview, StringComparison.Ordinal);
        Assert.Contains("new FileId()", preview, StringComparison.Ordinal);
        Assert.Contains("ResolveAssetAsync(", preview, StringComparison.Ordinal);
        Assert.Contains("LoadPreviewAsync(asset, cancellation.Token)", preview, StringComparison.Ordinal);
        Assert.True(
            CountOccurrences(templates, "new AssetPreviewImage") >= 2,
            "Both texture fields and general FileId fields must use the shared preview control.");
        Assert.Contains("AssetPreviewImage.FileIdProperty", templates, StringComparison.Ordinal);
        Assert.Contains("<controls:AssetPreviewImage", fileIdEditor, StringComparison.Ordinal);
        Assert.Contains("<controls:AssetPreviewImage", materialTemplate, StringComparison.Ordinal);
        Assert.Contains("TryGetCachedPreview(outAsset)", converter, StringComparison.Ordinal);
    }

    [Fact]
    public void Inspector_UsesParentScrollerForAnimations()
    {
        var template = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView",
            "ModelFileTemplate.xaml");

        Assert.Contains(
            "BindableLayout.ItemsSource=\"{Binding Animations}\"",
            template);
        Assert.DoesNotContain(
            "<CollectionView Grid.Row=\"16\"",
            template);
    }

    [Fact]
    public void Inspector_RoundTripsModelLodGenerationSettings()
    {
        var viewModel = ReadRepositoryFile(
            "Editor",
            "ViewModels",
            "ModelFile.cs");
        var template = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView",
            "ModelFileTemplate.xaml");

        foreach (var property in new[]
        {
            "ShouldGenerateLods",
            "NumGeneratedLods",
            "LodReductionFactor"
        })
        {
            Assert.Contains(property, viewModel, StringComparison.Ordinal);
            Assert.Contains($"Binding {property}", template, StringComparison.Ordinal);
        }

        Assert.Contains("case \"bGenerateLods\"", viewModel, StringComparison.Ordinal);
        Assert.Contains("case \"numGeneratedLods\"", viewModel, StringComparison.Ordinal);
        Assert.Contains("case \"lodReductionFactor\"", viewModel, StringComparison.Ordinal);
        Assert.Contains("new Scalar(null, \"bGenerateLods\")", viewModel, StringComparison.Ordinal);
        Assert.Contains("new Scalar(null, \"numGeneratedLods\")", viewModel, StringComparison.Ordinal);
        Assert.Contains("new Scalar(null, \"lodReductionFactor\")", viewModel, StringComparison.Ordinal);
    }

    static string ReadRepositoryFile(
        params string[] relativePath)
    {
        var current =
            new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var candidate = Path.Combine(
                relativePath
                    .Prepend(current.FullName)
                    .ToArray());
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate);
            }

            current = current.Parent;
        }

        throw new FileNotFoundException(
            $"Could not find repository file: " +
            $"{Path.Combine(relativePath)}");
    }

    static int CountOccurrences(string source, string value)
    {
        var count = 0;
        var index = 0;
        while ((index = source.IndexOf(value, index, StringComparison.Ordinal)) >= 0)
        {
            count++;
            index += value.Length;
        }

        return count;
    }
}
