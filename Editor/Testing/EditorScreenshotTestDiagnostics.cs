#nullable enable

using System.Text;

namespace SailorEditor.Testing;

internal static class EditorScreenshotTestDiagnostics
{
    const string StartupLogEnvironmentVariable = "SAILOR_EDITOR_STARTUP_LOG";
    static readonly object Sync = new();
    static readonly UTF8Encoding Utf8WithoutBom = new(false);

    public static bool IsEnabled => TryGetStartupLogPath(out _);

    public static void TryWriteCheckpoint(string source)
        => TryWrite(source, "Checkpoint reached.");

    public static void TryWriteUnhandledException(string source, object? exception)
        => TryWrite(source, exception switch
        {
            Exception error => $"{error.GetType().FullName} (HRESULT 0x{error.HResult:X8}){Environment.NewLine}{error}",
            null => "No exception object was provided.",
            _ => exception.ToString() ?? exception.GetType().FullName ?? "Unknown exception"
        });

    static void TryWrite(string source, string details)
    {
        try
        {
            if (!TryGetStartupLogPath(out var startupLogPath))
                return;

            var outputDirectory = Path.GetDirectoryName(startupLogPath);
            if (string.IsNullOrWhiteSpace(outputDirectory))
                return;

            Directory.CreateDirectory(outputDirectory);
            var entry = $"""
                {DateTimeOffset.UtcNow:O} {source}
                Process: {Environment.ProcessId}; managed thread: {Environment.CurrentManagedThreadId}
                Base directory: {AppContext.BaseDirectory}
                Current directory: {Environment.CurrentDirectory}
                {details}

                """;

            lock (Sync)
            {
                using var output = new FileStream(
                    startupLogPath,
                    FileMode.Append,
                    FileAccess.Write,
                    FileShare.ReadWrite);
                using var writer = new StreamWriter(output, Utf8WithoutBom, 1024, leaveOpen: true);
                writer.Write(entry);
                writer.Flush();
                output.Flush(flushToDisk: true);
            }
        }
        catch
        {
            // Diagnostics must never mask the original startup failure.
        }
    }

    static bool TryGetStartupLogPath(out string startupLogPath)
    {
        startupLogPath = string.Empty;
        try
        {
            var requestedPath = Environment.GetEnvironmentVariable(StartupLogEnvironmentVariable);
            if (string.IsNullOrWhiteSpace(requestedPath) || !Path.IsPathFullyQualified(requestedPath))
                return false;

            startupLogPath = Path.GetFullPath(requestedPath);
            return true;
        }
        catch
        {
            return false;
        }
    }
}
