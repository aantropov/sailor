#nullable enable

namespace SailorEditor.Testing;

internal sealed record EditorScreenshotTestOptions(
    string? ScreenshotPath,
    string? StateDirectory,
    string? ResultPath)
{
    const string ScreenshotArgument = "--editor-screenshot";
    const string ScreenshotArgumentPrefix = ScreenshotArgument + "=";
    const string StateDirectoryName = ".screenshot-state";
    const string ResultFileSuffix = ".result.json";

    public bool IsEnabled => ScreenshotPath is not null;

    public static EditorScreenshotTestOptions Parse(IEnumerable<string> arguments)
    {
        ArgumentNullException.ThrowIfNull(arguments);

        string? screenshotPath = null;
        var foundScreenshotArgument = false;

        foreach (var argument in arguments)
        {
            if (string.Equals(argument, ScreenshotArgument, StringComparison.Ordinal))
            {
                throw new ArgumentException(
                    $"{ScreenshotArgument} requires an absolute PNG output path.",
                    nameof(arguments));
            }

            if (argument is null ||
                !argument.StartsWith(ScreenshotArgumentPrefix, StringComparison.Ordinal))
            {
                continue;
            }

            if (foundScreenshotArgument)
            {
                throw new ArgumentException(
                    $"{ScreenshotArgument} may only be specified once.",
                    nameof(arguments));
            }

            foundScreenshotArgument = true;
            var requestedPath = argument[ScreenshotArgumentPrefix.Length..];
            if (string.IsNullOrWhiteSpace(requestedPath))
            {
                throw new ArgumentException(
                    $"{ScreenshotArgument} requires an absolute PNG output path.",
                    nameof(arguments));
            }

            if (!Path.IsPathFullyQualified(requestedPath))
            {
                throw new ArgumentException(
                    $"{ScreenshotArgument} output path must be absolute.",
                    nameof(arguments));
            }

            if (!string.Equals(Path.GetExtension(requestedPath), ".png", StringComparison.OrdinalIgnoreCase) ||
                string.IsNullOrWhiteSpace(Path.GetFileNameWithoutExtension(requestedPath)))
            {
                throw new ArgumentException(
                    $"{ScreenshotArgument} output path must name a PNG file.",
                    nameof(arguments));
            }

            screenshotPath = Path.GetFullPath(requestedPath);
        }

        if (screenshotPath is null)
        {
            return new EditorScreenshotTestOptions(null, null, null);
        }

        var outputDirectory = Path.GetDirectoryName(screenshotPath)
            ?? throw new ArgumentException(
                $"{ScreenshotArgument} output path must have a parent directory.",
                nameof(arguments));
        var resultFileName = Path.GetFileNameWithoutExtension(screenshotPath) + ResultFileSuffix;

        return new EditorScreenshotTestOptions(
            screenshotPath,
            Path.Combine(outputDirectory, StateDirectoryName),
            Path.Combine(outputDirectory, resultFileName));
    }
}
