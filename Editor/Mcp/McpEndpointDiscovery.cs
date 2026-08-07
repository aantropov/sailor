#nullable enable

using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;

namespace SailorEditor.Mcp;

public sealed record McpEndpointDescriptor(
    int ProtocolVersion,
    int ProcessId,
    int Port,
    string AuthenticationToken,
    string ProjectMode,
    string? WorkspacePath,
    DateTimeOffset StartedAtUtc);

public sealed record McpEndpointSelection(
    McpEndpointDescriptor? Endpoint,
    string? Error)
{
    public bool Succeeded => Endpoint is not null;
}

public sealed class McpEndpointDiscovery
{
    public const int CurrentProtocolVersion = 1;
    const string DescriptorExtension = ".json";

    static readonly JsonSerializerOptions s_jsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };

    readonly string _directoryPath;

    public McpEndpointDiscovery(string? directoryPath = null)
    {
        _directoryPath = string.IsNullOrWhiteSpace(directoryPath)
            ? GetDefaultDirectoryPath()
            : Path.GetFullPath(directoryPath);
    }

    public string DirectoryPath => _directoryPath;

    public string GetDescriptorPath(int processId) =>
        Path.Combine(_directoryPath, processId + DescriptorExtension);

    public static string CreateAuthenticationToken() =>
        Convert.ToBase64String(RandomNumberGenerator.GetBytes(32));

    public async Task WriteAsync(
        McpEndpointDescriptor descriptor,
        CancellationToken cancellationToken = default)
    {
        Validate(descriptor);
        Directory.CreateDirectory(_directoryPath);
        RestrictDirectoryPermissions(_directoryPath);

        var destinationPath = GetDescriptorPath(descriptor.ProcessId);
        var temporaryPath = destinationPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            var json = JsonSerializer.Serialize(descriptor, s_jsonOptions);
            await File.WriteAllTextAsync(temporaryPath, json, cancellationToken);
            RestrictFilePermissions(temporaryPath);
            File.Move(temporaryPath, destinationPath, true);
            RestrictFilePermissions(destinationPath);
        }
        finally
        {
            TryDelete(temporaryPath);
        }
    }

    public void Delete(int processId) => TryDelete(GetDescriptorPath(processId));

    public IReadOnlyList<McpEndpointDescriptor> ReadAvailable()
    {
        if (!Directory.Exists(_directoryPath))
            return Array.Empty<McpEndpointDescriptor>();

        var endpoints = new List<McpEndpointDescriptor>();
        foreach (var path in Directory.EnumerateFiles(
                     _directoryPath,
                     "*" + DescriptorExtension,
                     SearchOption.TopDirectoryOnly))
        {
            McpEndpointDescriptor? descriptor = null;
            try
            {
                descriptor = JsonSerializer.Deserialize<McpEndpointDescriptor>(
                    File.ReadAllText(path),
                    s_jsonOptions);
            }
            catch (JsonException)
            {
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }

            if (descriptor is null ||
                descriptor.ProtocolVersion != CurrentProtocolVersion ||
                descriptor.Port is < 1 or > 65535 ||
                string.IsNullOrWhiteSpace(descriptor.AuthenticationToken) ||
                !IsEndpointProcessAlive(descriptor))
            {
                TryDelete(path);
                continue;
            }

            endpoints.Add(descriptor);
        }

        return endpoints
            .OrderByDescending(endpoint => endpoint.StartedAtUtc)
            .ThenByDescending(endpoint => endpoint.ProcessId)
            .ToArray();
    }

    public McpEndpointSelection Select(
        int? processId = null,
        string? workspacePath = null)
    {
        var candidates = ReadAvailable().AsEnumerable();
        if (processId is not null)
            candidates = candidates.Where(endpoint => endpoint.ProcessId == processId.Value);

        if (!string.IsNullOrWhiteSpace(workspacePath))
        {
            var normalizedWorkspace = NormalizePath(workspacePath);
            candidates = candidates.Where(endpoint =>
                string.Equals(
                    NormalizePath(endpoint.WorkspacePath),
                    normalizedWorkspace,
                    PathComparison));
        }

        var matches = candidates.ToArray();
        if (matches.Length == 1)
            return new McpEndpointSelection(matches[0], null);

        if (matches.Length == 0)
        {
            var selector = processId is not null
                ? $" PID {processId.Value}"
                : !string.IsNullOrWhiteSpace(workspacePath)
                    ? $" workspace '{Path.GetFullPath(workspacePath)}'"
                    : string.Empty;
            return new McpEndpointSelection(
                null,
                "No running Sailor Editor endpoint matches" + selector + ".");
        }

        return new McpEndpointSelection(
            null,
            "Multiple Sailor Editor instances are running. Pass --pid or --workspace explicitly.");
    }

    static string GetDefaultDirectoryPath()
    {
        var configuredPath = Environment.GetEnvironmentVariable(
            "SAILOR_EDITOR_MCP_DISCOVERY");
        if (!string.IsNullOrWhiteSpace(configuredPath))
        {
            return Path.GetFullPath(configuredPath);
        }

        // MacCatalyst maps LocalApplicationData into its app container while the
        // stdio bridge is a normal console process. Use a stable per-user path so
        // both processes discover the same endpoint.
        var basePath = OperatingSystem.IsMacOS() || OperatingSystem.IsMacCatalyst()
            ? Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                ".sailor")
            : Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Sailor");
        return Path.Combine(basePath, "Editor", "Mcp");
    }

    static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;

    static string? NormalizePath(string? path) =>
        string.IsNullOrWhiteSpace(path)
            ? null
            : Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    static bool IsEndpointProcessAlive(McpEndpointDescriptor descriptor)
    {
        if (descriptor.ProcessId <= 0)
            return false;

        try
        {
            using var process = Process.GetProcessById(descriptor.ProcessId);
            if (process.HasExited)
                return false;

            // A live, unrelated process may have reused the PID of a crashed
            // Editor. Its start time must not be newer than the descriptor.
            return process.StartTime.ToUniversalTime() <=
                descriptor.StartedAtUtc.UtcDateTime.AddSeconds(5);
        }
        catch (ArgumentException)
        {
            return false;
        }
        catch (InvalidOperationException)
        {
            return false;
        }
        catch (System.ComponentModel.Win32Exception)
        {
            return false;
        }
        catch (PlatformNotSupportedException)
        {
            return true;
        }
    }

    static void Validate(McpEndpointDescriptor descriptor)
    {
        if (descriptor.ProtocolVersion != CurrentProtocolVersion)
            throw new ArgumentOutOfRangeException(nameof(descriptor), "Unsupported MCP endpoint protocol version.");
        if (descriptor.ProcessId <= 0)
            throw new ArgumentOutOfRangeException(nameof(descriptor), "A valid process id is required.");
        if (descriptor.Port is < 1 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(descriptor), "A valid loopback port is required.");
        if (string.IsNullOrWhiteSpace(descriptor.AuthenticationToken))
            throw new ArgumentException("An authentication token is required.", nameof(descriptor));
    }

    static void RestrictDirectoryPermissions(string path)
    {
        if (OperatingSystem.IsWindows())
            return;

        try
        {
            File.SetUnixFileMode(
                path,
                UnixFileMode.UserRead |
                UnixFileMode.UserWrite |
                UnixFileMode.UserExecute);
        }
        catch (PlatformNotSupportedException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        catch (IOException)
        {
        }
    }

    static void RestrictFilePermissions(string path)
    {
        if (OperatingSystem.IsWindows())
            return;

        try
        {
            File.SetUnixFileMode(
                path,
                UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
        catch (PlatformNotSupportedException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        catch (IOException)
        {
        }
    }

    static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
