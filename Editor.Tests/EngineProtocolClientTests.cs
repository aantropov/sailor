using Google.Protobuf;
using SailorEditor.Protocol;
using SailorEditor.Protocol.Generated;
using System.Runtime.InteropServices;

namespace Editor.Tests;

public sealed class EngineProtocolClientTests
{
    [Fact]
    public void Invoke_StampsVersionAndMonotonicRequestIds_AndFreesEveryResponse()
    {
        var requests = new List<ProtocolRequest>();
        var freeCount = 0;
        var client = CreateClient(
            request =>
            {
                requests.Add(request);
                return Success(
                    request,
                    response => response.BoolResult = new BoolResult { Value = true });
            },
            () => freeCount++);

        Assert.True(client.RequestAssetReload());
        Assert.True(client.RequestAssetReload());

        Assert.Equal(2, requests.Count);
        Assert.All(
            requests,
            request => Assert.Equal(EngineProtocolClient.ProtocolVersion, request.ProtocolVersion));
        Assert.Equal([1ul, 2ul], requests.Select(request => request.RequestId));
        Assert.All(
            requests,
            request => Assert.Equal(
                ProtocolRequest.CommandOneofCase.RequestAssetReload,
                request.CommandCase));
        Assert.Equal(2, freeCount);
    }

    [Fact]
    public void Invoke_FreesNativeBufferWhenResponseIsMalformed()
    {
        var freeCount = 0;
        var client = CreateRawClient(
            _ => [0xFF],
            () => freeCount++);

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains("malformed protobuf", exception.Message, StringComparison.Ordinal);
        Assert.Equal(1, freeCount);
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    [InlineData(5)]
    [InlineData(6)]
    public void Invoke_PropagatesTransportStatusWithoutFreeingNullBuffer(int status)
    {
        var freeCount = 0;

        int Invoke(
            byte[] requestData,
            uint requestSize,
            out nint responseData,
            out uint responseSize)
        {
            responseData = nint.Zero;
            responseSize = 0;
            return status;
        }

        var client = new EngineProtocolClient(
            Invoke,
            _ => freeCount++);

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains(
            $"transport status {status}",
            exception.Message,
            StringComparison.Ordinal);
        Assert.Equal(0, freeCount);
    }

    [Fact]
    public void Invoke_RejectsNullOrZeroSizedResponseAndOnlyFreesReturnedPointer()
    {
        var nullFreeCount = 0;

        int ReturnNull(
            byte[] requestData,
            uint requestSize,
            out nint responseData,
            out uint responseSize)
        {
            responseData = nint.Zero;
            responseSize = 0;
            return 0;
        }

        var nullClient = new EngineProtocolClient(
            ReturnNull,
            _ => nullFreeCount++);

        Assert.Throws<EngineProtocolException>(
            () => nullClient.RequestAssetReload());
        Assert.Equal(0, nullFreeCount);

        var returnedPointer = nint.Zero;
        var pointerFreeCount = 0;

        int ReturnZeroSize(
            byte[] requestData,
            uint requestSize,
            out nint responseData,
            out uint responseSize)
        {
            returnedPointer = Marshal.AllocHGlobal(1);
            responseData = returnedPointer;
            responseSize = 0;
            return 0;
        }

        void FreeReturnedPointer(nint buffer)
        {
            Assert.Equal(returnedPointer, buffer);
            pointerFreeCount++;
            Marshal.FreeHGlobal(buffer);
        }

        var zeroSizeClient = new EngineProtocolClient(
            ReturnZeroSize,
            FreeReturnedPointer);

        Assert.Throws<EngineProtocolException>(
            () => zeroSizeClient.RequestAssetReload());
        Assert.Equal(1, pointerFreeCount);
    }

    [Fact]
    public void Invoke_PropagatesProtocolErrorAndFreesResponseExactlyOnce()
    {
        var freeCount = 0;
        var client = CreateClient(
            request => new ProtocolResponse
            {
                ProtocolVersion = EngineProtocolClient.ProtocolVersion,
                RequestId = request.RequestId,
                Success = false,
                Error = "native command failed"
            },
            () => freeCount++);

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Equal("native command failed", exception.Message);
        Assert.Equal(1, freeCount);
    }

    [Fact]
    public void Invoke_RejectsUnexpectedResultOneofAfterFreeingResponse()
    {
        var freeCount = 0;
        var client = CreateClient(
            request => Success(
                request,
                response => response.StringResult = new StringResult
                {
                    HasValue = true,
                    Value = "wrong result"
                }),
            () => freeCount++);

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains("unexpected result type", exception.Message, StringComparison.Ordinal);
        Assert.Equal(1, freeCount);
    }

    [Theory]
    [InlineData(true, false, "protocol version")]
    [InlineData(false, true, "request id")]
    public void Invoke_RejectsMismatchedResponseEnvelopeAndStillFrees(
        bool wrongVersion,
        bool wrongRequestId,
        string expectedError)
    {
        var freeCount = 0;
        var client = CreateClient(
            request =>
            {
                var response = Success(
                    request,
                    value => value.BoolResult = new BoolResult { Value = true });
                if (wrongVersion)
                {
                    response.ProtocolVersion++;
                }
                if (wrongRequestId)
                {
                    response.RequestId++;
                }
                return response;
            },
            () => freeCount++);

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains(expectedError, exception.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Equal(1, freeCount);
    }

    [Fact]
    public void SetEditorSelection_UsesTypedRepeatedInstanceIds()
    {
        ProtocolRequest? captured = null;
        var client = CreateClient(
            request =>
            {
                captured = request;
                return Success(
                    request,
                    response => response.BoolResult = new BoolResult { Value = true });
            });

        Assert.True(client.SetEditorSelection(["go-1", "component-2"]));

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.SetEditorSelection,
            captured.CommandCase);
        Assert.Equal(["go-1", "component-2"], captured.SetEditorSelection.InstanceIds);
    }

    [Fact]
    public void SerializeEngineTypes_UsesDedicatedProtocolCommand()
    {
        ProtocolRequest? captured = null;
        var client = CreateClient(
            request =>
            {
                captured = request;
                return Success(
                    request,
                    response => response.StringResult = new StringResult
                    {
                        HasValue = true,
                        Value = "EngineTypes: []"
                    });
            });

        Assert.Equal("EngineTypes: []", client.SerializeEngineTypes());

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.SerializeEngineTypes,
            captured.CommandCase);
    }

    [Fact]
    public void ProtocolStrings_RejectEmbeddedNullBeforeInvokingNativeTransport()
    {
        var invokeCount = 0;

        int Invoke(
            byte[] requestData,
            uint requestSize,
            out nint responseData,
            out uint responseSize)
        {
            invokeCount++;
            responseData = nint.Zero;
            responseSize = 0;
            return 1;
        }

        var client = new EngineProtocolClient(Invoke, _ => { });
        Action[] invalidCalls =
        [
            () => client.Initialize(["SailorEditor", "workspace\0suffix"]),
            () => client.LoadEditorWorld("file\0id"),
            () => client.GetEditorManagedMutationRevision(0, "object\0id"),
            () => client.UpdateObject("object\0id", "{}"),
            () => client.UpdateObject("object", "yaml\0changes"),
            () => client.ReparentObject("object\0id", "parent", true),
            () => client.ReparentObject("object", "parent\0id", true),
            () => client.CreateGameObject("parent\0id", "preferred"),
            () => client.CreateGameObject("parent", "preferred\0id"),
            () => client.DestroyObject("object\0id"),
            () => client.ResetComponentToDefaults("component\0id"),
            () => client.AddComponent("object\0id", "MeshRenderer", "preferred"),
            () => client.AddComponent("object", "Mesh\0Renderer", "preferred"),
            () => client.AddComponent("object", "MeshRenderer", "preferred\0id"),
            () => client.RemoveComponent("component\0id"),
            () => client.InstantiatePrefab("file\0id", "parent"),
            () => client.InstantiatePrefab("file", "parent\0id"),
            () => client.InstantiatePrefabFromYaml("yaml\0value", "parent"),
            () => client.InstantiatePrefabFromYaml("yaml", "parent\0id"),
            () => client.SetEditorSelection(["object", "component\0id"]),
            () => client.RenderPathTracedImage("output\0path", "object", 1, 1, 1),
            () => client.RenderPathTracedImage("output", "object\0id", 1, 1, 1)
        ];

        foreach (var invalidCall in invalidCalls)
        {
            var exception = Assert.Throws<ArgumentException>(invalidCall);
            Assert.Contains("embedded null", exception.Message, StringComparison.Ordinal);
        }

        Assert.Equal(0, invokeCount);
    }

    [Fact]
    public void Initialize_RejectsNullArgumentBeforeInvokingNativeTransport()
    {
        var invokeCount = 0;

        int Invoke(
            byte[] requestData,
            uint requestSize,
            out nint responseData,
            out uint responseSize)
        {
            invokeCount++;
            responseData = nint.Zero;
            responseSize = 0;
            return 1;
        }

        var client = new EngineProtocolClient(Invoke, _ => { });

        var exception = Assert.Throws<ArgumentException>(
            () => client.Initialize(new string[] { "SailorEditor", null! }));

        Assert.Contains("must not be null", exception.Message, StringComparison.Ordinal);
        Assert.Equal(0, invokeCount);
    }

    static EngineProtocolClient CreateClient(
        Func<ProtocolRequest, ProtocolResponse> responder,
        Action? onFree = null)
        => CreateRawClient(
            requestData =>
            {
                var request = ProtocolRequest.Parser.ParseFrom(requestData);
                return responder(request).ToByteArray();
            },
            onFree);

    static EngineProtocolClient CreateRawClient(
        Func<byte[], byte[]> responder,
        Action? onFree = null)
    {
        int Invoke(
            byte[] requestData,
            uint requestSize,
            out nint responseData,
            out uint responseSize)
        {
            Assert.Equal((uint)requestData.Length, requestSize);
            var response = responder(requestData);
            responseSize = checked((uint)response.Length);
            responseData = Marshal.AllocHGlobal(response.Length);
            Marshal.Copy(response, 0, responseData, response.Length);
            return 0;
        }

        void Free(nint buffer)
        {
            onFree?.Invoke();
            Marshal.FreeHGlobal(buffer);
        }

        return new EngineProtocolClient(Invoke, Free);
    }

    static ProtocolResponse Success(
        ProtocolRequest request,
        Action<ProtocolResponse> setResult)
    {
        var response = new ProtocolResponse
        {
            ProtocolVersion = EngineProtocolClient.ProtocolVersion,
            RequestId = request.RequestId,
            Success = true
        };
        setResult(response);
        return response;
    }
}
