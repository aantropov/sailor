#nullable enable

using System.Diagnostics;
using System.Text.Json;
using Microsoft.Maui.Media;

namespace SailorEditor.Testing;

internal sealed class EditorScreenshotTestRunner
{
    static readonly TimeSpan CaptureDelay = TimeSpan.FromSeconds(5);
    static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };

    readonly EditorScreenshotTestOptions _options;
    int _started;

    public EditorScreenshotTestRunner(EditorScreenshotTestOptions options)
    {
        _options = options;
    }

    public async Task CaptureAndExitAsync(CancellationToken cancellationToken = default)
    {
        if (!_options.IsEnabled || Interlocked.Exchange(ref _started, 1) != 0)
            return;

        var readyAtUtc = DateTimeOffset.UtcNow;
        var stopwatch = Stopwatch.StartNew();
        string? temporaryScreenshotPath = null;

        try
        {
            if (Application.Current?.Windows.FirstOrDefault() is { } window)
            {
                window.Width = 1024;
                window.Height = 768;
            }

            Directory.CreateDirectory(_options.StateDirectory!);
            Directory.CreateDirectory(Path.GetDirectoryName(_options.ScreenshotPath!)!);
            DeleteIfExists(_options.ScreenshotPath!);
            DeleteIfExists(_options.ResultPath!);

            await Task.Delay(CaptureDelay, cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();

            if (!Screenshot.Default.IsCaptureSupported)
                throw new InvalidOperationException("MAUI screenshot capture is not supported on this Windows runner.");

            var screenshot = await Screenshot.Default.CaptureAsync();
            if (screenshot.Width < 800 || screenshot.Height < 600)
            {
                throw new InvalidOperationException(
                    $"Captured screenshot dimensions are too small: {screenshot.Width}x{screenshot.Height}; expected at least 800x600.");
            }

            temporaryScreenshotPath = $"{_options.ScreenshotPath}.{Guid.NewGuid():N}.tmp";
            await using (var output = new FileStream(
                temporaryScreenshotPath,
                FileMode.CreateNew,
                FileAccess.Write,
                FileShare.None,
                81920,
                FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await screenshot.CopyToAsync(output, ScreenshotFormat.Png, 100);
                await output.FlushAsync(cancellationToken);
            }

            var byteCount = new FileInfo(temporaryScreenshotPath).Length;
            if (byteCount < 4096)
                throw new InvalidOperationException($"Captured PNG is unexpectedly small: {byteCount} bytes.");

            File.Move(temporaryScreenshotPath, _options.ScreenshotPath!, true);
            temporaryScreenshotPath = null;

            var capturedAtUtc = DateTimeOffset.UtcNow;
            await WriteResultAsync(new EditorScreenshotTestResult(
                true,
                _options.ScreenshotPath!,
                readyAtUtc,
                capturedAtUtc,
                stopwatch.ElapsedMilliseconds,
                screenshot.Width,
                screenshot.Height,
                byteCount,
                null));

            Console.WriteLine(
                $"Editor screenshot test captured {_options.ScreenshotPath} ({screenshot.Width}x{screenshot.Height}, {byteCount} bytes) after {stopwatch.ElapsedMilliseconds} ms.");
            ExitApplication(0);
        }
        catch (Exception ex)
        {
            if (temporaryScreenshotPath is not null)
                DeleteIfExists(temporaryScreenshotPath);

            Console.Error.WriteLine($"Editor screenshot test failed: {ex}");
            await TryWriteFailureResultAsync(readyAtUtc, stopwatch.ElapsedMilliseconds, ex);
            ExitApplication(1);
        }
    }

    async Task WriteResultAsync(EditorScreenshotTestResult result)
    {
        var temporaryResultPath = $"{_options.ResultPath}.{Guid.NewGuid():N}.tmp";
        try
        {
            var json = JsonSerializer.Serialize(result, JsonOptions);
            await File.WriteAllTextAsync(temporaryResultPath, json);
            File.Move(temporaryResultPath, _options.ResultPath!, true);
        }
        finally
        {
            DeleteIfExists(temporaryResultPath);
        }
    }

    async Task TryWriteFailureResultAsync(DateTimeOffset readyAtUtc, long delayMilliseconds, Exception exception)
    {
        try
        {
            var resultDirectory = Path.GetDirectoryName(_options.ResultPath!);
            if (!string.IsNullOrWhiteSpace(resultDirectory))
                Directory.CreateDirectory(resultDirectory);

            await WriteResultAsync(new EditorScreenshotTestResult(
                false,
                _options.ScreenshotPath!,
                readyAtUtc,
                null,
                delayMilliseconds,
                null,
                null,
                null,
                exception.ToString()));
        }
        catch (Exception resultException)
        {
            Console.Error.WriteLine($"Could not write screenshot test result: {resultException}");
        }
    }

    static void ExitApplication(int exitCode)
    {
        Environment.ExitCode = exitCode;
        try
        {
            if (Application.Current is not null)
            {
                Application.Current.Quit();
                return;
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Could not close the screenshot test application cleanly: {ex}");
        }

        Environment.Exit(exitCode);
    }

    static void DeleteIfExists(string path)
    {
        if (File.Exists(path))
            File.Delete(path);
    }

    sealed record EditorScreenshotTestResult(
        bool Success,
        string PngPath,
        DateTimeOffset ReadyAtUtc,
        DateTimeOffset? CapturedAtUtc,
        long DelayMilliseconds,
        int? Width,
        int? Height,
        long? ByteCount,
        string? Error);
}
