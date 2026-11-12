#nullable enable

using System.Diagnostics;
using System.Text;

namespace SailorEditor.Workspace;

public sealed record WorkspaceProcessInvocation(
    string FileName,
    IReadOnlyList<string> Arguments,
    string WorkingDirectory)
{
    public Encoding? OutputEncoding { get; init; }
}

public sealed record WorkspaceProcessResult(
    int ExitCode,
    string Output,
    TimeSpan Duration)
{
    public bool Succeeded => ExitCode == 0;
}

public interface IWorkspaceProcessRunner
{
    Task<WorkspaceProcessResult> RunAsync(
        WorkspaceProcessInvocation invocation,
        CancellationToken cancellationToken = default);
}

public sealed class WorkspaceProcessRunner : IWorkspaceProcessRunner
{
    const int MaximumCapturedCharacters = 1_000_000;

    public async Task<WorkspaceProcessResult> RunAsync(
        WorkspaceProcessInvocation invocation,
        CancellationToken cancellationToken = default)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = invocation.FileName,
            WorkingDirectory = invocation.WorkingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = invocation.OutputEncoding,
            StandardErrorEncoding = invocation.OutputEncoding,
            CreateNoWindow = true,
        };
        foreach (var argument in invocation.Arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = new Process { StartInfo = startInfo };
        var stopwatch = Stopwatch.StartNew();
        try
        {
            if (!process.Start())
            {
                throw new InvalidOperationException($"Unable to start '{invocation.FileName}'.");
            }
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            return new WorkspaceProcessResult(
                -1,
                exception.Message,
                stopwatch.Elapsed);
        }

        using var cancellationRegistration = cancellationToken.Register(() =>
        {
            try
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }
            }
            catch (InvalidOperationException)
            {
            }
        });

        var standardOutput = process.StandardOutput.ReadToEndAsync();
        var standardError = process.StandardError.ReadToEndAsync();
        try
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            // Export cleanup must wait until the child releases staged files.
            await process.WaitForExitAsync(CancellationToken.None);
            await Task.WhenAll(standardOutput, standardError);
            throw;
        }
        var output = await standardOutput;
        var error = await standardError;
        stopwatch.Stop();

        var combined = new StringBuilder();
        if (!string.IsNullOrWhiteSpace(output))
        {
            combined.AppendLine(output.TrimEnd());
        }
        if (!string.IsNullOrWhiteSpace(error))
        {
            combined.AppendLine(error.TrimEnd());
        }
        if (combined.Length > MaximumCapturedCharacters)
        {
            combined.Remove(0, combined.Length - MaximumCapturedCharacters);
            combined.Insert(0, "[Output truncated to the final 1,000,000 characters]\n");
        }

        return new WorkspaceProcessResult(
            process.ExitCode,
            combined.ToString(),
            stopwatch.Elapsed);
    }
}

