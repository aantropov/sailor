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
}
