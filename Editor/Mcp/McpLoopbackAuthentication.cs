#nullable enable

using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace SailorEditor.Mcp;

public static class McpLoopbackAuthentication
{
    const int MaximumHandshakeBytes = 4096;

    sealed record AuthenticationRequest(string Token);

    public static async Task WriteRequestAsync(
        Stream stream,
        string authenticationToken,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (string.IsNullOrWhiteSpace(authenticationToken))
            throw new ArgumentException("An authentication token is required.", nameof(authenticationToken));

        var payload = JsonSerializer.SerializeToUtf8Bytes(
            new AuthenticationRequest(authenticationToken));
        await stream.WriteAsync(payload, cancellationToken);
        await stream.WriteAsync("\n"u8.ToArray(), cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    public static async Task<bool> ValidateRequestAsync(
        Stream stream,
        string expectedAuthenticationToken,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (string.IsNullOrWhiteSpace(expectedAuthenticationToken))
            return false;

        var payload = await ReadLineAsync(stream, cancellationToken);
        if (payload is null)
            return false;

        AuthenticationRequest? request;
        try
        {
            request = JsonSerializer.Deserialize<AuthenticationRequest>(payload);
        }
        catch (JsonException)
        {
            return false;
        }

        if (string.IsNullOrWhiteSpace(request?.Token))
            return false;

        var expected = Encoding.UTF8.GetBytes(expectedAuthenticationToken);
        var actual = Encoding.UTF8.GetBytes(request.Token);
        return expected.Length == actual.Length &&
            CryptographicOperations.FixedTimeEquals(expected, actual);
    }

    static async Task<byte[]?> ReadLineAsync(
        Stream stream,
        CancellationToken cancellationToken)
    {
        using var payload = new MemoryStream();
        var oneByte = new byte[1];
        while (payload.Length < MaximumHandshakeBytes)
        {
            var read = await stream.ReadAsync(oneByte, cancellationToken);
            if (read == 0)
                return null;
            if (oneByte[0] == (byte)'\n')
                return payload.ToArray();
            if (oneByte[0] != (byte)'\r')
                payload.WriteByte(oneByte[0]);
        }

        return null;
    }
}
