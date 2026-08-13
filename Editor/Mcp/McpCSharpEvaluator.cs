#nullable enable

using System.Diagnostics;
using System.Reflection;
using System.Runtime.Loader;
using System.Security;
using System.Text;

namespace SailorEditor.Mcp;

public sealed record McpCSharpEvalResult(
    bool Succeeded,
    string? Result,
    string? ResultType,
    string Output,
    string? Error);

internal sealed class McpCSharpEvaluator
{
    const int MaxCodeLength = 64 * 1024;
    readonly IServiceProvider _services;
    readonly IEditorThreadDispatcher _editorThread;

    public McpCSharpEvaluator(
        IServiceProvider services,
        IEditorThreadDispatcher editorThread)
    {
        _services = services;
        _editorThread = editorThread;
    }

    public async Task<McpCSharpEvalResult> EvaluateAsync(
        string code,
        int timeoutMs,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(code))
            return new(false, null, null, string.Empty, "C# code must not be empty.");
        if (code.Length > MaxCodeLength)
            return new(false, null, null, string.Empty, $"C# code is limited to {MaxCodeLength} characters.");

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(Math.Clamp(timeoutMs, 100, 30_000));
        var temporaryRoot = Path.Combine(
            Path.GetTempPath(),
            "SailorEditorMcpEval",
            Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporaryRoot);
        try
        {
            await File.WriteAllTextAsync(
                Path.Combine(temporaryRoot, "Eval.csproj"),
                CreateProjectSource(),
                timeout.Token);
            await File.WriteAllTextAsync(
                Path.Combine(temporaryRoot, "Script.cs"),
                CreateScriptSource(code),
                timeout.Token);

            var build = await BuildAsync(temporaryRoot, timeout.Token);
            if (!build.Succeeded)
                return new(false, null, null, build.Output, build.Error);

            return await ExecuteAsync(
                Path.Combine(temporaryRoot, "bin", "Release", "net10.0", "SailorMcpEval.dll"),
                timeout.Token);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new(false, null, null, string.Empty, "C# evaluation timed out.");
        }
        catch (Exception exception)
        {
            return new(false, null, null, string.Empty, exception.ToString());
        }
        finally
        {
            try { Directory.Delete(temporaryRoot, true); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
    }

    async Task<McpCSharpEvalResult> ExecuteAsync(
        string assemblyPath,
        CancellationToken cancellationToken)
    {
        var output = new StringBuilder();
        var loadContext = new AssemblyLoadContext(
            "SailorMcpEval_" + Guid.NewGuid().ToString("N"),
            isCollectible: true);
        loadContext.Resolving += ResolveEditorAssembly;
        try
        {
            var assembly = loadContext.LoadFromAssemblyPath(assemblyPath);
            var method = assembly.GetType("SailorMcpEval.ScriptEntry")?
                .GetMethod("EvaluateAsync", BindingFlags.Public | BindingFlags.Static);
            if (method is null)
                return new(false, null, null, string.Empty, "Compiled C# script has no entry point.");

            var result = await _editorThread.InvokeAsync<object?>(
                async () =>
                {
                    var task = method.Invoke(
                        null,
                        [_services, _editorThread, cancellationToken, (Action<object?>)(value => output.AppendLine(value?.ToString() ?? "null"))]) as Task<object?>;
                    if (task is null)
                        throw new InvalidOperationException("Compiled C# script returned an invalid task.");
                    return await task;
                },
                cancellationToken);
            return new(true, result?.ToString(), result?.GetType().FullName, output.ToString().TrimEnd(), null);
        }
        catch (TargetInvocationException exception)
        {
            return new(false, null, null, output.ToString().TrimEnd(), (exception.InnerException ?? exception).ToString());
        }
        catch (Exception exception)
        {
            return new(false, null, null, output.ToString().TrimEnd(), exception.ToString());
        }
        finally
        {
            loadContext.Resolving -= ResolveEditorAssembly;
            loadContext.Unload();
        }
    }

    static Assembly? ResolveEditorAssembly(AssemblyLoadContext _, AssemblyName name) =>
        AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(assembly =>
            AssemblyName.ReferenceMatchesDefinition(assembly.GetName(), name));

    static async Task<(bool Succeeded, string Output, string? Error)> BuildAsync(
        string workingDirectory,
        CancellationToken cancellationToken)
    {
        using var process = Process.Start(new ProcessStartInfo
        {
            FileName = "dotnet",
            Arguments = "build Eval.csproj --configuration Release --nologo --verbosity quiet",
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        });
        if (process is null)
            return (false, string.Empty, "Failed to start the dotnet C# compiler.");

        var output = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var error = process.StandardError.ReadToEndAsync(cancellationToken);
        try
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            TryKill(process);
            throw;
        }

        var standardOutput = await output;
        var standardError = await error;
        return process.ExitCode == 0
            ? (true, standardOutput.Trim(), null)
            : (false, standardOutput.Trim(), string.IsNullOrWhiteSpace(standardError) ? standardOutput.Trim() : standardError.Trim());
    }

    static string CreateProjectSource()
    {
        var editorAssembly = SecurityElement.Escape(typeof(McpCSharpEvaluator).Assembly.Location);
        return $$"""
            <Project Sdk="Microsoft.NET.Sdk">
              <PropertyGroup>
                <TargetFramework>net10.0</TargetFramework>
                <ImplicitUsings>enable</ImplicitUsings>
                <Nullable>enable</Nullable>
                <AssemblyName>SailorMcpEval</AssemblyName>
              </PropertyGroup>
              <ItemGroup>
                <Reference Include="SailorEditor"><HintPath>{{editorAssembly}}</HintPath><Private>false</Private></Reference>
              </ItemGroup>
            </Project>
            """;
    }

    static string CreateScriptSource(string code) => $$"""
        namespace SailorMcpEval;

        public static class ScriptEntry
        {
            public static async Task<object?> EvaluateAsync(
                IServiceProvider Services,
                SailorEditor.Mcp.IEditorThreadDispatcher EditorThread,
                CancellationToken CancellationToken,
                Action<object?> Print)
            {
        #line 1 "MCP C# eval"
        {{code}}
        #line default
            }
        }
        """;

    static void TryKill(Process process)
    {
        try { process.Kill(true); }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
    }
}
