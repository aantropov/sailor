using SailorEditor.Testing;

namespace Editor.Tests;

public sealed class EditorScreenshotTestOptionsTests
{
    [Fact]
    public void Parse_DisablesScreenshotModeWhenOptionIsAbsent()
    {
        var options = EditorScreenshotTestOptions.Parse(
            ["SailorEditor.exe", "--workspace=/tmp/project", "--editor"]);

        Assert.False(options.IsEnabled);
        Assert.Null(options.ScreenshotPath);
        Assert.Null(options.StateDirectory);
        Assert.Null(options.ResultPath);
    }

    [Fact]
    public void Parse_NormalizesOutputAndDerivesAdjacentAutomationPaths()
    {
        var requestedPath = Path.Combine(
            Path.GetTempPath(),
            "sailor-screenshot-tests",
            "intermediate",
            "..",
            "captures",
            "Editor Main.PNG");
        var expectedPath = Path.GetFullPath(requestedPath);
        var expectedDirectory = Path.GetDirectoryName(expectedPath)!;

        var options = EditorScreenshotTestOptions.Parse(
            ["SailorEditor.exe", "--unrelated", $"--editor-screenshot={requestedPath}"]);

        Assert.True(options.IsEnabled);
        Assert.Equal(expectedPath, options.ScreenshotPath);
        Assert.Equal(Path.Combine(expectedDirectory, ".screenshot-state"), options.StateDirectory);
        Assert.Equal(Path.Combine(expectedDirectory, "Editor Main.result.json"), options.ResultPath);
    }

    [Theory]
    [InlineData("--editor-screenshot")]
    [InlineData("--editor-screenshot=")]
    [InlineData("--editor-screenshot=   ")]
    public void Parse_RejectsMissingOutputPath(string argument)
    {
        var error = Assert.Throws<ArgumentException>(
            () => EditorScreenshotTestOptions.Parse(["SailorEditor.exe", argument]));

        Assert.Contains("absolute PNG output path", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Parse_RejectsRelativeOutputPath()
    {
        var error = Assert.Throws<ArgumentException>(
            () => EditorScreenshotTestOptions.Parse(
                ["SailorEditor.exe", $"--editor-screenshot={Path.Combine("captures", "editor.png")}"]));

        Assert.Contains("must be absolute", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Parse_RejectsDuplicateOption()
    {
        var firstPath = Path.Combine(Path.GetTempPath(), "first.png");
        var secondPath = Path.Combine(Path.GetTempPath(), "second.png");

        var error = Assert.Throws<ArgumentException>(
            () => EditorScreenshotTestOptions.Parse(
            [
                "SailorEditor.exe",
                $"--editor-screenshot={firstPath}",
                $"--editor-screenshot={secondPath}"
            ]));

        Assert.Contains("may only be specified once", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Parse_RejectsNonPngOutputPath()
    {
        var outputPath = Path.Combine(Path.GetTempPath(), "editor.jpg");

        var error = Assert.Throws<ArgumentException>(
            () => EditorScreenshotTestOptions.Parse(
                ["SailorEditor.exe", $"--editor-screenshot={outputPath}"]));

        Assert.Contains("must name a PNG file", error.Message, StringComparison.Ordinal);
    }
}
