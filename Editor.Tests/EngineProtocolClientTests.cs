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
    public void WebSocketTransport_PreservesCallerCancellation()
    {
        using var transport =
            new ClientWebSocketEngineProtocolTransport(
                new Uri("ws://127.0.0.1:4000/sailor/editor/v1"),
                ValidAuthorizationToken);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        var exception = Assert.Throws<OperationCanceledException>(() =>
            transport.Invoke(
                [1],
                EngineProtocolInvocationKind.Request,
                cancellation.Token));

        Assert.Equal(cancellation.Token, exception.CancellationToken);
    }

    [Fact]
    public async Task WebSocketTransport_DisposeDoesNotWaitForeverForHeldLaneGate()
    {
        var transport =
            new ClientWebSocketEngineProtocolTransport(
                new Uri("ws://127.0.0.1:4000/sailor/editor/v1"),
                ValidAuthorizationToken);
        var laneField = typeof(ClientWebSocketEngineProtocolTransport)
            .GetField(
                "longRunningLane",
                System.Reflection.BindingFlags.Instance |
                    System.Reflection.BindingFlags.NonPublic);
        Assert.NotNull(laneField);
        var lane = laneField.GetValue(transport);
        Assert.NotNull(lane);
        var gateProperty = lane.GetType().GetProperty("Gate");
        Assert.NotNull(gateProperty);
        var gate = Assert.IsType<SemaphoreSlim>(
            gateProperty.GetValue(lane));
        Assert.True(gate.Wait(0));

        var disposeTask = Task.CompletedTask;
        try
        {
            disposeTask = Task.Run(transport.Dispose);
            await disposeTask.WaitAsync(TimeSpan.FromSeconds(2));
        }
        finally
        {
            gate.Release();
        }

        await disposeTask.WaitAsync(TimeSpan.FromSeconds(2));
        transport.Dispose();
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
    public void Invoke_StampsVersionAndMonotonicRequestIds()
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

        Assert.True(client.RequestAssetReload());
        Assert.True(client.RequestAssetReload());

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
    public void Invoke_RejectsMalformedTransportResponse()
    {
        var client = CreateRawClient(_ => [0xFF]);

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains(
            "malformed protobuf",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Invoke_PropagatesTransportFailure()
    {
        var client = new EngineProtocolClient((_, _) =>
            throw new EngineProtocolException("transport disconnected"));

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Equal("transport disconnected", exception.Message);
    }

    [Fact]
    public void Invoke_RejectsEmptyTransportResponse()
    {
        var client = CreateRawClient(_ => []);

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains(
            "invalid response",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Invoke_PropagatesProtocolError()
    {
        var client = CreateClient(request => new ProtocolResponse
        {
            ProtocolVersion = EngineProtocolClient.ProtocolVersion,
            RequestId = request.RequestId,
            Success = false,
            Error = "engine command failed"
        });

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Equal("engine command failed", exception.Message);
    }

    [Fact]
    public void Invoke_RejectsUnexpectedResultOneof()
    {
        var client = CreateClient(request => Success(
            request,
            response => response.StringResult = new StringResult
            {
                HasValue = true,
                Value = "wrong result"
            }));

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains(
            "unexpected result type",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(true, false, "protocol version")]
    [InlineData(false, true, "request id")]
    public void Invoke_RejectsMismatchedResponseEnvelope(
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

        var exception = Assert.Throws<EngineProtocolException>(
            () => client.RequestAssetReload());

        Assert.Contains(
            expectedError,
            exception.Message,
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Start_UsesIndependentLongRunningTransportLane()
    {
        EngineProtocolInvocationKind? capturedKind = null;
        var client = CreateClient(
            request => Success(
                request,
                response => response.EmptyResult = new Empty()),
            kind => capturedKind = kind);

        client.Start();

        Assert.Equal(
            EngineProtocolInvocationKind.LongRunning,
            capturedKind);
    }

    [Fact]
    public void Start_ForwardsSessionCancellationToTheLongRunningLane()
    {
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var transport = new CancellationRecordingTransport();
        using var client = new EngineProtocolClient(transport);

        var exception = Assert.Throws<OperationCanceledException>(() =>
            client.Start(cancellation.Token));

        Assert.Equal(
            EngineProtocolInvocationKind.LongRunning,
            transport.InvocationKind);
        Assert.Equal(cancellation.Token, transport.CancellationToken);
        Assert.Equal(cancellation.Token, exception.CancellationToken);
    }

    [Fact]
    public void Stop_UsesBoundedLifecycleTransportTimeout()
    {
        EngineProtocolInvocationKind? capturedKind = null;
        var client = CreateClient(
            request => Success(
                request,
                response => response.EmptyResult = new Empty()),
            kind => capturedKind = kind);

        client.Stop();

        Assert.Equal(
            EngineProtocolInvocationKind.Lifecycle,
            capturedKind);
    }

    [Fact]
    public void ViewportInput_UsesBoundedInteractiveTransportTimeout()
    {
        EngineProtocolInvocationKind? capturedKind = null;
        var client = CreateClient(
            request => Success(
                request,
                response => response.BoolResult =
                    new BoolResult { Value = true }),
            kind => capturedKind = kind);

        Assert.True(client.SendRemoteViewportInput(
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
    public void Initialize_UsesTypedRepeatedArguments()
    {
        ProtocolRequest? captured = null;
        var client = CreateClient(request =>
        {
            captured = request;
            return Success(
                request,
                response => response.EmptyResult = new Empty());
        });

        client.Initialize(["SailorEditor", "--workspace", "/tmp/Project"]);

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.Initialize,
            captured.CommandCase);
        Assert.Equal(
            ["SailorEditor", "--workspace", "/tmp/Project"],
            captured.Initialize.Arguments);
    }

    [Fact]
    public void EngineReadiness_UsesDedicatedMainThreadProbe()
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

        Assert.True(client.IsEngineMainThreadReady());

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.IsEngineMainThreadReady,
            captured.CommandCase);
    }

    [Fact]
    public void SetEditorSelection_UsesTypedRepeatedInstanceIds()
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
            client.SetEditorSelection(["go-1", "component-2"]));

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.SetEditorSelection,
            captured.CommandCase);
        Assert.Equal(
            ["go-1", "component-2"],
            captured.SetEditorSelection.InstanceIds);
    }

    [Fact]
    public void SerializeEngineTypes_UsesDedicatedProtocolCommand()
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
            client.SerializeEngineTypes());

        Assert.NotNull(captured);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.SerializeEngineTypes,
            captured.CommandCase);
    }

    [Fact]
    public void ProtocolStrings_RejectEmbeddedNullBeforeInvokingTransport()
    {
        var invokeCount = 0;
        var client = new EngineProtocolClient((_, _) =>
        {
            invokeCount++;
            throw new EngineProtocolException(
                "Transport should not be invoked.");
        });
        Action[] invalidCalls =
        [
            () => client.Initialize(
                ["SailorEditor", "workspace\0suffix"]),
            () => client.LoadEditorWorld("file\0id"),
            () => client.GetEditorManagedMutationRevision(
                0,
                "object\0id"),
            () => client.UpdateObject("object\0id", "{}"),
            () => client.UpdateObject("object", "yaml\0changes"),
            () => client.ReparentObject(
                "object\0id",
                "parent",
                true),
            () => client.ReparentObject(
                "object",
                "parent\0id",
                true),
            () => client.CreateGameObject(
                "parent\0id",
                "preferred"),
            () => client.CreateGameObject(
                "parent",
                "preferred\0id"),
            () => client.DestroyObject("object\0id"),
            () => client.ResetComponentToDefaults(
                "component\0id"),
            () => client.AddComponent(
                "object\0id",
                "MeshRenderer",
                "preferred"),
            () => client.AddComponent(
                "object",
                "Mesh\0Renderer",
                "preferred"),
            () => client.AddComponent(
                "object",
                "MeshRenderer",
                "preferred\0id"),
            () => client.RemoveComponent("component\0id"),
            () => client.InstantiatePrefab(
                "file\0id",
                "parent"),
            () => client.InstantiatePrefab(
                "file",
                "parent\0id"),
            () => client.InstantiatePrefabFromYaml(
                "yaml\0value",
                "parent"),
            () => client.InstantiatePrefabFromYaml(
                "yaml",
                "parent\0id"),
            () => client.SetEditorSelection(
                ["object", "component\0id"]),
            () => client.RenderPathTracedImage(
                "output\0path",
                "object",
                1,
                1,
                1),
            () => client.RenderPathTracedImage(
                "output",
                "object\0id",
                1,
                1,
                1)
        ];

        foreach (var invalidCall in invalidCalls)
        {
            var exception = Assert.Throws<ArgumentException>(
                invalidCall);
            Assert.Contains(
                "embedded null",
                exception.Message,
                StringComparison.Ordinal);
        }

        Assert.Equal(0, invokeCount);
    }

    [Fact]
    public void Initialize_RejectsNullArgumentBeforeInvokingTransport()
    {
        var invokeCount = 0;
        var client = new EngineProtocolClient((_, _) =>
        {
            invokeCount++;
            return [];
        });

        var exception = Assert.Throws<ArgumentException>(
            () => client.Initialize(
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
        => new((requestData, invocationKind) =>
        {
            onInvoke?.Invoke(invocationKind);
            return responder(requestData);
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

        public byte[] Invoke(
            byte[] requestData,
            EngineProtocolInvocationKind invocationKind,
            CancellationToken cancellationToken = default)
        {
            InvocationKind = invocationKind;
            CancellationToken = cancellationToken;
            cancellationToken.ThrowIfCancellationRequested();
            throw new InvalidOperationException(
                "A cancelled request must not reach the responder.");
        }

        public void Dispose()
        {
        }
    }
}
