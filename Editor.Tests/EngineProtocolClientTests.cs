using Google.Protobuf;
using SailorEditor.Protocol;
using SailorEditor.Protocol.Generated;

namespace Editor.Tests;

public sealed class EngineProtocolClientTests
{
    const string ValidAuthorizationToken =
        "0123456789ABCDEF0123456789ABCDEF";

    [Theory]
    [InlineData("http://127.0.0.1:4000/sailor/editor/v1")]
    [InlineData("ws://192.0.2.1:4000/sailor/editor/v1")]
    [InlineData("ws://127.0.0.1:4000/wrong")]
    [InlineData("ws://user@127.0.0.1:4000/sailor/editor/v1")]
    [InlineData("ws://127.0.0.1:4000/sailor/editor/v1?channel=editor")]
    [InlineData("ws://127.0.0.1:4000/sailor/editor/v1#fragment")]
    public void WebSocketTransport_RejectsUnsafeOrInvalidEndpoint(
        string endpoint)
    {
        Assert.Throws<ArgumentException>(() =>
            new ClientWebSocketEngineProtocolTransport(
                new Uri(endpoint),
                ValidAuthorizationToken));
    }

    [Fact]
    public void WebSocketTransport_AcceptsLoopbackWsAndRemoteWss()
    {
        using var loopback =
            new ClientWebSocketEngineProtocolTransport(
                new Uri("ws://127.0.0.1:4000/sailor/editor/v1"),
                ValidAuthorizationToken);
        using var remote =
            new ClientWebSocketEngineProtocolTransport(
                new Uri("wss://engine.example.test/sailor/editor/v1"),
                ValidAuthorizationToken);
    }

    [Fact]
    public void WebSocketTransport_DoesNotReserveAConnectionForBlockingStart()
    {
        Assert.DoesNotContain(
            "LongRunning",
            Enum.GetNames<EngineProtocolInvocationKind>());
        Assert.Null(
            typeof(ClientWebSocketEngineProtocolTransport).GetField(
                "longRunningLane",
                System.Reflection.BindingFlags.Instance |
                    System.Reflection.BindingFlags.NonPublic));
    }

    [Fact]
    public async Task WebSocketTransport_PreservesCallerCancellation()
    {
        using var transport =
            new ClientWebSocketEngineProtocolTransport(
                new Uri("ws://127.0.0.1:4000/sailor/editor/v1"),
                ValidAuthorizationToken);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        var exception = await Assert.ThrowsAsync<OperationCanceledException>(
            () => transport.InvokeAsync(
                [1],
                EngineProtocolInvocationKind.Request,
                cancellation.Token));

        Assert.Equal(cancellation.Token, exception.CancellationToken);
    }

    [Fact]
    public async Task WebSocketTransport_DisposeIsNonBlockingAndAsyncDisposeDrains()
    {
        var transport =
            new ClientWebSocketEngineProtocolTransport(
                new Uri("ws://127.0.0.1:4000/sailor/editor/v1"),
                ValidAuthorizationToken);

        transport.Dispose();
        await transport.DisposeAsync()
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(2));
    }

    [Fact]
    public async Task DisposeAsync_AwaitsTransportDrain()
    {
        var transport = new AsyncDisposeRecordingTransport();
        var client = new EngineProtocolClient(transport);

        var disposal = client.DisposeAsync().AsTask();

        Assert.Equal(1, transport.DisposeCount);
        Assert.False(disposal.IsCompleted);
        transport.CompleteDisposal();
        await disposal.WaitAsync(TimeSpan.FromSeconds(2));
    }

    [Theory]
    [InlineData("")]
    [InlineData("short")]
    [InlineData("0123456789ABCDEF0123456789ABCDE\n")]
    public void WebSocketTransport_RejectsInvalidAuthorizationToken(
        string token)
    {
        Assert.Throws<ArgumentException>(() =>
            new ClientWebSocketEngineProtocolTransport(
                new Uri("ws://127.0.0.1:4000/sailor/editor/v1"),
                token));
    }

    [Fact]
    public async Task InvokeAsync_StampsVersionAndMonotonicRequestIds()
    {
        var requests = new List<ProtocolRequest>();
        var client = CreateClient(request =>
        {
            requests.Add(request);
            return Success(
                request,
                response => response.BoolResult =
                    new BoolResult { Value = true });
        });

        Assert.True(await client.RequestAssetReloadAsync());
        Assert.True(await client.RequestAssetReloadAsync());

        Assert.Equal(2, requests.Count);
        Assert.All(
            requests,
            request => Assert.Equal(
                EngineProtocolClient.ProtocolVersion,
                request.ProtocolVersion));
        Assert.Equal(
            [1ul, 2ul],
            requests.Select(request => request.RequestId));
        Assert.All(
            requests,
            request => Assert.Equal(
                ProtocolRequest.CommandOneofCase.RequestAssetReload,
                request.CommandCase));
    }

    [Fact]
    public async Task InvokeAsync_RejectsMalformedTransportResponse()
    {
        var client = CreateRawClient(_ => [0xFF]);

        var exception = await Assert.ThrowsAsync<EngineProtocolException>(
            () => client.RequestAssetReloadAsync());

        Assert.Contains(
            "malformed protobuf",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task InvokeAsync_PropagatesTransportFailure()
    {
        var client = new EngineProtocolClient((_, _, _) =>
            Task.FromException<byte[]>(
                new EngineProtocolException(
                    "transport disconnected")));

        var exception = await Assert.ThrowsAsync<EngineProtocolException>(
            () => client.RequestAssetReloadAsync());

        Assert.Equal("transport disconnected", exception.Message);
    }

    [Fact]
    public async Task InvokeAsync_RejectsEmptyTransportResponse()
    {
        var client = CreateRawClient(_ => []);

        var exception = await Assert.ThrowsAsync<EngineProtocolException>(
            () => client.RequestAssetReloadAsync());

        Assert.Contains(
            "invalid response",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task InvokeAsync_PropagatesProtocolError()
    {
        var client = CreateClient(request => new ProtocolResponse
        {
            ProtocolVersion = EngineProtocolClient.ProtocolVersion,
            RequestId = request.RequestId,
            Success = false,
            Error = "engine command failed"
        });

        var exception = await Assert.ThrowsAsync<EngineProtocolException>(
            () => client.RequestAssetReloadAsync());

        Assert.Equal("engine command failed", exception.Message);
    }

    [Fact]
    public async Task InvokeAsync_RejectsUnexpectedResultOneof()
    {
        var client = CreateClient(request => Success(
            request,
            response => response.StringResult = new StringResult
            {
                HasValue = true,
                Value = "wrong result"
            }));

        var exception = await Assert.ThrowsAsync<EngineProtocolException>(
            () => client.RequestAssetReloadAsync());

        Assert.Contains(
            "unexpected result type",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(true, false, "protocol version")]
    [InlineData(false, true, "request id")]
    public async Task InvokeAsync_RejectsMismatchedResponseEnvelope(
        bool wrongVersion,
        bool wrongRequestId,
        string expectedError)
    {
        var client = CreateClient(request =>
        {
            var response = Success(
                request,
                value => value.BoolResult =
                    new BoolResult { Value = true });
            if (wrongVersion)
            {
                response.ProtocolVersion++;
            }
            if (wrongRequestId)
            {
                response.RequestId++;
            }
            return response;
        });

        var exception = await Assert.ThrowsAsync<EngineProtocolException>(
            () => client.RequestAssetReloadAsync());

        Assert.Contains(
            expectedError,
            exception.Message,
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task StartAsync_UsesBoundedLifecycleTransportLane()
    {
        EngineProtocolInvocationKind? capturedKind = null;
        var client = CreateClient(
            request => Success(
                request,
                response => response.EmptyResult = new Empty()),
            kind => capturedKind = kind);

        await client.StartAsync();

        Assert.Equal(
            EngineProtocolInvocationKind.Lifecycle,
            capturedKind);
    }

    [Fact]
    public async Task StartAsync_ForwardsSessionCancellationToTheLifecycleLane()
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var transport = new CancellationRecordingTransport();
        using var client = new EngineProtocolClient(transport);

        var exception = await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => client.StartAsync(cancellation.Token));

        Assert.Equal(
            EngineProtocolInvocationKind.Lifecycle,
            transport.InvocationKind);
        Assert.Equal(cancellation.Token, transport.CancellationToken);
        Assert.Equal(cancellation.Token, exception.CancellationToken);
    }

    [Fact]
    public async Task StopAsync_UsesBoundedLifecycleTransportTimeout()
    {
        EngineProtocolInvocationKind? capturedKind = null;
        var client = CreateClient(
            request => Success(
                request,
                response => response.EmptyResult = new Empty()),
            kind => capturedKind = kind);

        await client.StopAsync();

        Assert.Equal(
            EngineProtocolInvocationKind.Lifecycle,
            capturedKind);
    }

    [Fact]
    public async Task ViewportInputAsync_UsesBoundedInteractiveTransportTimeout()
    {
        EngineProtocolInvocationKind? capturedKind = null;
        var client = CreateClient(
            request => Success(
                request,
                response => response.BoolResult =
                    new BoolResult { Value = true }),
            kind => capturedKind = kind);

        Assert.True(await client.SendRemoteViewportInputAsync(
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            false,
            true,
            false));

        Assert.Equal(
            EngineProtocolInvocationKind.Interactive,
            capturedKind);
    }

    [Fact]
    public async Task InitializeAsync_UsesTypedRepeatedArguments()
    {
        ProtocolRequest? captured = null;
        var client = CreateClient(request =>
        {
            captured = request;
            return Success(
                request,
                response => response.EmptyResult = new Empty());
        });

        await client.InitializeAsync(
            ["SailorEditor", "--workspace", "/tmp/Project"]);

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.Initialize,
            captured.CommandCase);
        Assert.Equal(
            ["SailorEditor", "--workspace", "/tmp/Project"],
            captured.Initialize.Arguments);
    }

    [Fact]
    public async Task EngineReadinessAsync_UsesDedicatedMainThreadProbe()
    {
        ProtocolRequest? captured = null;
        var client = CreateClient(request =>
        {
            captured = request;
            return Success(
                request,
                response => response.BoolResult =
                    new BoolResult { Value = true });
        });

        Assert.True(await client.IsEngineMainThreadReadyAsync());

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.IsEngineMainThreadReady,
            captured.CommandCase);
    }

    [Fact]
    public async Task EngineLivenessAsync_UsesDedicatedLifecycleProbe()
    {
        ProtocolRequest? captured = null;
        EngineProtocolInvocationKind? capturedKind = null;
        var client = CreateClient(
            request =>
            {
                captured = request;
                return Success(
                    request,
                    response => response.BoolResult =
                        new BoolResult { Value = true });
            },
            kind => capturedKind = kind);

        Assert.True(await client.IsEngineRunningAsync());

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.IsEngineRunning,
            captured.CommandCase);
        Assert.Equal(
            EngineProtocolInvocationKind.Lifecycle,
            capturedKind);
    }

    [Fact]
    public async Task SetEditorSelectionAsync_UsesTypedRepeatedInstanceIds()
    {
        ProtocolRequest? captured = null;
        var client = CreateClient(request =>
        {
            captured = request;
            return Success(
                request,
                response => response.BoolResult =
                    new BoolResult { Value = true });
        });

        Assert.True(
            await client.SetEditorSelectionAsync(
                ["go-1", "component-2"]));

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.SetEditorSelection,
            captured.CommandCase);
        Assert.Equal(
            ["go-1", "component-2"],
            captured.SetEditorSelection.InstanceIds);
    }

    [Fact]
    public async Task SerializeEngineTypesAsync_UsesDedicatedProtocolCommand()
    {
        ProtocolRequest? captured = null;
        var client = CreateClient(request =>
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

        Assert.Equal(
            "EngineTypes: []",
            await client.SerializeEngineTypesAsync());

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.SerializeEngineTypes,
            captured.CommandCase);
    }

    [Fact]
    public async Task ProtocolStrings_RejectEmbeddedNullBeforeInvokingTransport()
    {
        var invokeCount = 0;
        var client = new EngineProtocolClient((_, _, _) =>
        {
            invokeCount++;
            return Task.FromException<byte[]>(
                new EngineProtocolException(
                    "Transport should not be invoked."));
        });
        Func<Task>[] invalidCalls =
        [
            () => client.InitializeAsync(
                ["SailorEditor", "workspace\0suffix"]),
            () => client.LoadEditorWorldAsync("file\0id"),
            () => client.GetEditorManagedMutationRevisionAsync(
                0,
                "object\0id"),
            () => client.UpdateObjectAsync("object\0id", "{}"),
            () => client.UpdateObjectAsync("object", "yaml\0changes"),
            () => client.ReparentObjectAsync(
                "object\0id",
                "parent",
                true),
            () => client.ReparentObjectAsync(
                "object",
                "parent\0id",
                true),
            () => client.CreateGameObjectAsync(
                "parent\0id",
                "preferred"),
            () => client.CreateGameObjectAsync(
                "parent",
                "preferred\0id"),
            () => client.DestroyObjectAsync("object\0id"),
            () => client.ResetComponentToDefaultsAsync(
                "component\0id"),
            () => client.AddComponentAsync(
                "object\0id",
                "MeshRenderer",
                "preferred"),
            () => client.AddComponentAsync(
                "object",
                "Mesh\0Renderer",
                "preferred"),
            () => client.AddComponentAsync(
                "object",
                "MeshRenderer",
                "preferred\0id"),
            () => client.RemoveComponentAsync("component\0id"),
            () => client.InstantiatePrefabAsync(
                "file\0id",
                "parent"),
            () => client.InstantiatePrefabAsync(
                "file",
                "parent\0id"),
            () => client.InstantiatePrefabFromYamlAsync(
                "yaml\0value",
                "parent"),
            () => client.InstantiatePrefabFromYamlAsync(
                "yaml",
                "parent\0id"),
            () => client.SetEditorSelectionAsync(
                ["object", "component\0id"]),
            () => client.RenderPathTracedImageAsync(
                "output\0path",
                "object",
                1,
                1,
                1),
            () => client.RenderPathTracedImageAsync(
                "output",
                "object\0id",
                1,
                1,
                1)
        ];

        foreach (var invalidCall in invalidCalls)
        {
            var exception = await Assert.ThrowsAsync<ArgumentException>(
                invalidCall);
            Assert.Contains(
                "embedded null",
                exception.Message,
                StringComparison.Ordinal);
        }

        Assert.Equal(0, invokeCount);
    }

    [Fact]
    public async Task InitializeAsync_RejectsNullArgumentBeforeInvokingTransport()
    {
        var invokeCount = 0;
        var client = new EngineProtocolClient((_, _, _) =>
        {
            invokeCount++;
            return Task.FromResult<byte[]>([]);
        });

        var exception = await Assert.ThrowsAsync<ArgumentException>(
            () => client.InitializeAsync(
                new string[] { "SailorEditor", null! }));

        Assert.Contains(
            "must not be null",
            exception.Message,
            StringComparison.Ordinal);
        Assert.Equal(0, invokeCount);
    }

    static EngineProtocolClient CreateClient(
        Func<ProtocolRequest, ProtocolResponse> responder,
        Action<EngineProtocolInvocationKind>? onInvoke = null)
        => CreateRawClient(
            requestData =>
            {
                var request =
                    ProtocolRequest.Parser.ParseFrom(requestData);
                return responder(request).ToByteArray();
            },
            onInvoke);

    static EngineProtocolClient CreateRawClient(
        Func<byte[], byte[]> responder,
        Action<EngineProtocolInvocationKind>? onInvoke = null)
        => new((requestData, invocationKind, _) =>
        {
            onInvoke?.Invoke(invocationKind);
            return Task.FromResult(responder(requestData));
        });

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

    sealed class CancellationRecordingTransport : IEngineProtocolTransport
    {
        public EngineProtocolInvocationKind? InvocationKind { get; private set; }
        public CancellationToken CancellationToken { get; private set; }

        public Task<byte[]> InvokeAsync(
            byte[] requestData,
            EngineProtocolInvocationKind invocationKind,
            CancellationToken cancellationToken = default)
        {
            InvocationKind = invocationKind;
            CancellationToken = cancellationToken;
            return cancellationToken.IsCancellationRequested
                ? Task.FromCanceled<byte[]>(cancellationToken)
                : Task.FromException<byte[]>(
                    new InvalidOperationException(
                        "A cancelled request must not reach the responder."));
        }

        public void Dispose()
        {
        }

        public ValueTask DisposeAsync()
            => ValueTask.CompletedTask;
    }

    sealed class AsyncDisposeRecordingTransport :
        IEngineProtocolTransport,
        IAsyncDisposable
    {
        readonly TaskCompletionSource<bool> disposalCompletion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        int disposeCount;

        public int DisposeCount => Volatile.Read(ref disposeCount);

        public Task<byte[]> InvokeAsync(
            byte[] requestData,
            EngineProtocolInvocationKind invocationKind,
            CancellationToken cancellationToken = default)
            => Task.FromResult<byte[]>([]);

        public void Dispose()
            => Interlocked.Increment(ref disposeCount);

        public ValueTask DisposeAsync()
        {
            Dispose();
            return new ValueTask(disposalCompletion.Task);
        }

        public void CompleteDisposal()
            => disposalCompletion.TrySetResult(true);
    }
}
