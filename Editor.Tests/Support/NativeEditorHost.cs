using System.Diagnostics;
using System.Globalization;
using SailorEditor.Protocol;
using Xunit.Abstractions;

namespace Editor.Tests;

public sealed class NativeEditorFactAttribute : FactAttribute
{
    public NativeEditorFactAttribute()
    {
        if (string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("SAILOR_NATIVE_TEST_HOST")))
            Skip = "Set SAILOR_NATIVE_TEST_HOST to the built VulkanSubmissionTests executable.";
    }
}

// The child calls the production native exports on its own main thread.
// Only process bootstrap is adapted; protocol traffic uses the real socket.
internal sealed class NativeEditorHost(string workspace, string engine, ITestOutputHelper log) : ILocalEngineProtocolNativeBridge, IDisposable
{
    readonly string directory = Directory.CreateDirectory(Path.Combine(workspace, "NativeHost")).FullName;
    readonly List<int> processIds = [];
    Process? process;
    Task<string>? output;
    Task<string>? errors;

    public IReadOnlyList<int> ProcessIds => processIds;
    public int ExitCode => process!.ExitCode;
    public string Output => (output?.GetAwaiter().GetResult() ?? "") + (errors?.GetAwaiter().GetResult() ?? "");

    public int StartLocalHost(byte[] requestData, ushort port, string authorizationToken)
    {
        if (process is not null)
        {
            if (!process.WaitForExit(15000))
                throw new TimeoutException("The previous native host attempt did not finish.");
            log.WriteLine(Output);
            process.Dispose();
        }
        File.Delete(Path.Combine(directory, "ready"));
        File.Delete(Path.Combine(directory, "stop"));
        File.WriteAllBytes(Path.Combine(directory, "initialize.pb"), requestData);
        File.WriteAllText(Path.Combine(directory, "endpoint.yaml"), $"port: {port}\ntoken: {authorizationToken}\n");
        var start = new ProcessStartInfo(Path.GetFullPath(Environment.GetEnvironmentVariable("SAILOR_NATIVE_TEST_HOST")!))
        {
            WorkingDirectory = engine,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add("--gpu-editor-protocol-host");
        start.ArgumentList.Add(directory);
        if (OperatingSystem.IsMacOS()) start.Environment["DYLD_PRINT_LIBRARIES"] = "1";
        process = Process.Start(start) ?? throw new InvalidOperationException("The native test host did not start.");
        processIds.Add(process.Id);
        log.WriteLine("Native editor host PID: {0}", process.Id);
        output = process.StandardOutput.ReadToEndAsync();
        errors = process.StandardError.ReadToEndAsync();
        var ready = Path.Combine(directory, "ready");
        var deadline = Stopwatch.StartNew();
        while (!File.Exists(ready) && !process.HasExited && deadline.Elapsed < TimeSpan.FromSeconds(60))
            Thread.Sleep(10);
        if (File.Exists(ready)) return int.Parse(File.ReadAllText(ready), CultureInfo.InvariantCulture);
        if (!process.HasExited) process.Kill(entireProcessTree: true);
        process.WaitForExit();
        throw new InvalidOperationException($"Native editor host did not initialize.\n{Output}");
    }

    public void RequestLocalHostStop() => File.WriteAllText(Path.Combine(directory, "stop"), "");

    public void StopLocalHost(bool shutdownEngine)
    {
        RequestLocalHostStop();
        if (process is null) return;
        if (!process.WaitForExit(15000))
        {
            process.Kill(entireProcessTree: true);
            process.WaitForExit();
            throw new TimeoutException($"Native editor host did not shut down.\n{Output}");
        }
        if (process.ExitCode != 0) throw new InvalidOperationException($"Native editor host failed.\n{Output}");
    }

    public void Dispose()
    {
        try { if (process is not null && !process.HasExited) StopLocalHost(true); }
        finally
        {
            try { log.WriteLine(Output); }
            finally { process?.Dispose(); }
        }
    }
}
