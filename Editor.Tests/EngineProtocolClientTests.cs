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
    public async Task UpdateAssetAsync_SendsExactFileId()
    {
        ProtocolRequest? capturedRequest = null;
        var client = CreateClient(request =>
        {
            capturedRequest = request;
            return Success(
                request,
                response => response.BoolResult =
                    new BoolResult { Value = true });
        });
        const string fileId = "{01234567-89AB-CDEF-0123-456789ABCDEF}";

        Assert.True(await client.UpdateAssetAsync(fileId));

        Assert.NotNull(capturedRequest);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.UpdateAsset,
            capturedRequest.CommandCase);
        Assert.Equal(fileId, capturedRequest.UpdateAsset.FileId);
    }

    [Fact]
    public async Task AnimatorParameters_UseTypedProtocolValues()
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

        Assert.True(await client.SetAnimatorFloatAsync("component", "Speed", 1.5f));
        Assert.True(await client.SetAnimatorIntAsync("component", "Mode", 2));
        Assert.True(await client.SetAnimatorBoolAsync("component", "Grounded", true));
        Assert.True(await client.SetAnimatorTriggerAsync("component", "Jump", false));
        Assert.True(await client.SetAnimatorTriggerAsync("component", "Jump", true));

        Assert.All(requests, request => Assert.Equal(
            ProtocolRequest.CommandOneofCase.SetAnimatorParameter,
            request.CommandCase));
        Assert.Equal(
            [
                AnimatorParameterRequest.ValueOneofCase.FloatValue,
                AnimatorParameterRequest.ValueOneofCase.IntValue,
                AnimatorParameterRequest.ValueOneofCase.BoolValue,
                AnimatorParameterRequest.ValueOneofCase.Trigger,
                AnimatorParameterRequest.ValueOneofCase.ResetTrigger
            ],
            requests.Select(request => request.SetAnimatorParameter.ValueCase));
        Assert.Equal(1.5f, requests[0].SetAnimatorParameter.FloatValue);
        Assert.Equal(2, requests[1].SetAnimatorParameter.IntValue);
        Assert.True(requests[2].SetAnimatorParameter.BoolValue);
    }

    [Fact]
    public async Task GetAnimatorStateAsync_ReadsTransitionState()
    {
        var client = CreateClient(request => Success(
            request,
            response => response.AnimatorStateResult = new AnimatorStateResult
            {
                HasController = true,
                ControllerRevision = 7,
                ActiveStateId = 11,
                ActiveStateName = "Idle",
                ActiveStateTime = 0.5f,
                Transitioning = true,
                DestinationStateId = 12,
                DestinationStateName = "Run",
                DestinationStateTime = 0.1f,
                TransitionAlpha = 0.25f
            }));

        var state = await client.GetAnimatorStateAsync("component");

        Assert.True(state.HasController);
        Assert.Equal(7ul, state.ControllerRevision);
        Assert.Equal(11ul, state.ActiveStateId);
        Assert.Equal("Idle", state.ActiveStateName);
        Assert.True(state.IsTransitioning);
        Assert.Equal(12ul, state.DestinationStateId);
        Assert.Equal("Run", state.DestinationStateName);
        Assert.Equal(0.25f, state.TransitionAlpha);
    }

    [Fact]
    public async Task CreateModelInstanceAsync_SendsAtomicCreationRequest()
    {
        ProtocolRequest? capturedRequest = null;
        var client = CreateClient(request =>
        {
            capturedRequest = request;
            return Success(
                request,
                response => response.InstanceIdResult =
                    new InstanceIdResult
                    {
                        Succeeded = true,
                        InstanceId = "ROOT-ID"
                    });
        });
        var worldPosition = new EngineProtocolVector4(
            1.0f,
            2.0f,
            3.0f,
            1.0f);

        var result = await client.CreateModelInstanceAsync(
            "MODEL-ID",
            "Bistro",
            "PARENT-ID",
            createHierarchy: true,
            worldPosition,
            "ROOT-ID");

        Assert.True(result.Succeeded);
        Assert.Equal("ROOT-ID", result.InstanceId);
        Assert.NotNull(capturedRequest);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.CreateModelInstance,
            capturedRequest.CommandCase);
        var request = capturedRequest.CreateModelInstance;
        Assert.Equal("MODEL-ID", request.ModelFileId);
        Assert.Equal("Bistro", request.Name);
        Assert.Equal("PARENT-ID", request.ParentInstanceId);
        Assert.True(request.CreateHierarchy);
        Assert.True(request.ApplyWorldPosition);
        Assert.Equal(1.0f, request.WorldPosition.X);
        Assert.Equal(2.0f, request.WorldPosition.Y);
        Assert.Equal(3.0f, request.WorldPosition.Z);
        Assert.Equal(1.0f, request.WorldPosition.W);
        Assert.Equal("ROOT-ID", request.PreferredInstanceId);
    }

    [Fact]
    public async Task CreateModelInstanceAsync_FlatModeOmitsWorldPosition()
    {
        ProtocolRequest? capturedRequest = null;
        var client = CreateClient(request =>
        {
            capturedRequest = request;
            return Success(
                request,
                response => response.InstanceIdResult =
                    new InstanceIdResult());
        });

        await client.CreateModelInstanceAsync(
            "MODEL-ID",
            "Bistro",
            string.Empty,
            createHierarchy: false,
            worldPosition: null,
            "ROOT-ID");

        Assert.NotNull(capturedRequest);
        var request = capturedRequest.CreateModelInstance;
        Assert.False(request.CreateHierarchy);
        Assert.False(request.ApplyWorldPosition);
        Assert.Null(request.WorldPosition);
    }

    [Fact]
    public async Task CreateModelInstanceAsync_RejectsInvalidWorldPosition()
    {
        var invokeCount = 0;
        var client = CreateClient(request =>
        {
            invokeCount++;
            return Success(
                request,
                response => response.EmptyResult = new Empty());
        });

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            client.CreateModelInstanceAsync(
                "MODEL-ID",
                "Bistro",
                string.Empty,
                createHierarchy: true,
                new EngineProtocolVector4(
                    float.NaN,
                    0.0f,
                    0.0f,
                    1.0f),
                "ROOT-ID"));

        Assert.Equal(0, invokeCount);
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
    public async Task InitializeAsync_LocalTransportNegotiatesCapabilitiesOverHost()
    {
        var transport = new LocalCapabilityRecordingTransport();
        var client = new EngineProtocolClient(transport);

        await client.InitializeAsync(["SailorEditor"]);
        Assert.True(
            await client.InstantiatePrefabFromYamlStrictAsync(
                "prefab: undo",
                string.Empty));

        Assert.Single(transport.InitializeRequests);
        Assert.Equal(
            ProtocolRequest.CommandOneofCase.Initialize,
            transport.InitializeRequests[0].CommandCase);
        Assert.Equal(
            EngineProtocolClient.ProtocolVersion,
            transport.InitializeRequests[0].ProtocolVersion);
        Assert.Collection(
            transport.InvokedRequests,
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.GetExitCode,
                    request.CommandCase);
                Assert.Equal(
                    EngineProtocolClient.ProtocolVersion,
                    request.ProtocolVersion);
            },
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.InstantiatePrefabFromYaml,
                    request.CommandCase);
                Assert.Equal(
                    EngineProtocolClient.StrictInstanceIdsProtocolVersion,
                    request.ProtocolVersion);
                Assert.True(
                    request.InstantiatePrefabFromYaml.StrictInstanceIds);
            });
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
    public async Task InstantiatePrefabFromYamlAsync_UsesVersionGatedStrictRequest()
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

        Assert.True(
            await client.InstantiatePrefabFromYamlAsync(
                "prefab: default",
                string.Empty));
        Assert.True(
            await client.InstantiatePrefabFromYamlStrictAsync(
                "prefab: undo",
                "parent"));

        Assert.Collection(
            requests,
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.InstantiatePrefabFromYaml,
                    request.CommandCase);
                Assert.Equal(
                    EngineProtocolClient.ProtocolVersion,
                    request.ProtocolVersion);
                Assert.Equal(
                    "prefab: default",
                    request.InstantiatePrefabFromYaml.PrefabYaml);
                Assert.False(
                    request.InstantiatePrefabFromYaml.StrictInstanceIds);
            },
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.InstantiatePrefabFromYaml,
                    request.CommandCase);
                Assert.Equal(
                    EngineProtocolClient.StrictInstanceIdsProtocolVersion,
                    request.ProtocolVersion);
                Assert.Equal(
                    "prefab: undo",
                    request.InstantiatePrefabFromYaml.PrefabYaml);
                Assert.Equal(
                    "parent",
                    request.InstantiatePrefabFromYaml.ParentInstanceId);
                Assert.True(
                    request.InstantiatePrefabFromYaml.StrictInstanceIds);
            });
    }

    [Fact]
    public async Task InstantiatePrefabFromYamlStrictAsync_ProbesCapabilityBeforeColdRequest()
    {
        var requests = new List<ProtocolRequest>();
        var client = CreateClient(request =>
        {
            requests.Add(request);
            return request.CommandCase switch
            {
                ProtocolRequest.CommandOneofCase.GetExitCode =>
                    Success(
                        request,
                        response => response.Int32Result =
                            new Int32Result { Value = 0 }),
                ProtocolRequest.CommandOneofCase.InstantiatePrefabFromYaml =>
                    Success(
                        request,
                        response => response.BoolResult =
                            new BoolResult { Value = true }),
                _ => throw new InvalidOperationException(
                    $"Unexpected command {request.CommandCase}.")
            };
        });

        Assert.True(
            await client.InstantiatePrefabFromYamlStrictAsync(
                "prefab: undo",
                "parent"));

        Assert.Collection(
            requests,
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.GetExitCode,
                    request.CommandCase);
                Assert.Equal(
                    EngineProtocolClient.ProtocolVersion,
                    request.ProtocolVersion);
            },
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.InstantiatePrefabFromYaml,
                    request.CommandCase);
                Assert.Equal(
                    EngineProtocolClient.StrictInstanceIdsProtocolVersion,
                    request.ProtocolVersion);
                Assert.True(
                    request.InstantiatePrefabFromYaml.StrictInstanceIds);
            });
    }

    [Fact]
    public async Task InstantiatePrefabFromYamlStrictAsync_RejectsOldV1HostWithoutSendingMutation()
    {
        var requests = new List<ProtocolRequest>();
        var client = CreateClient(request =>
        {
            requests.Add(request);
            return Success(
                request,
                response =>
                {
                    if (request.CommandCase ==
                        ProtocolRequest.CommandOneofCase.Initialize)
                    {
                        response.EmptyResult = new Empty();
                    }
                    else
                    {
                        response.BoolResult =
                            new BoolResult { Value = true };
                    }
                },
                supportsStrictInstanceIds: false);
        });

        await client.InitializeAsync(["SailorEditor"]);
        Assert.True(
            await client.InstantiatePrefabFromYamlAsync(
                "prefab: legacy",
                string.Empty));

        var exception =
            await Assert.ThrowsAsync<EngineProtocolException>(
                () => client.InstantiatePrefabFromYamlStrictAsync(
                    "prefab: undo",
                    string.Empty));
        Assert.Contains(
            "does not advertise strict",
            exception.Message,
            StringComparison.Ordinal);
        Assert.Equal(2, requests.Count);
        Assert.DoesNotContain(
            requests,
            request =>
                request.CommandCase ==
                    ProtocolRequest.CommandOneofCase.InstantiatePrefabFromYaml &&
                request.InstantiatePrefabFromYaml.StrictInstanceIds);
    }

    [Fact]
    public async Task InstantiatePrefabFromYamlStrictAsync_RejectsV1ResponseAfterStaleCapability()
    {
        var requests = new List<ProtocolRequest>();
        var client = CreateClient(request =>
        {
            requests.Add(request);
            if (request.CommandCase ==
                ProtocolRequest.CommandOneofCase.RequestAssetReload)
            {
                return Success(
                    request,
                    response => response.BoolResult =
                        new BoolResult { Value = true });
            }

            return new ProtocolResponse
            {
                ProtocolVersion = EngineProtocolClient.ProtocolVersion,
                RequestId = request.RequestId,
                Success = true,
                BoolResult = new BoolResult { Value = true }
            };
        });

        Assert.True(await client.RequestAssetReloadAsync());
        var exception =
            await Assert.ThrowsAsync<EngineProtocolException>(
                () => client.InstantiatePrefabFromYamlStrictAsync(
                    "prefab: undo",
                    string.Empty));

        Assert.Contains(
            "protocol version",
            exception.Message,
            StringComparison.Ordinal);
        Assert.Equal(2, requests.Count);
        Assert.Equal(
            EngineProtocolClient.StrictInstanceIdsProtocolVersion,
            requests[1].ProtocolVersion);
        Assert.True(
            requests[1].InstantiatePrefabFromYaml.StrictInstanceIds);
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
    public async Task EditorControlCommands_UseTypedPayloadsAndExpectedLanes()
    {
        var requests = new List<ProtocolRequest>();
        var lanes = new List<EngineProtocolInvocationKind>();
        var client = CreateClient(
            request =>
            {
                requests.Add(request);
                return request.CommandCase switch
                {
                    ProtocolRequest.CommandOneofCase.TraceViewportRay =>
                        Success(
                            request,
                            response => response.Vector4Result =
                                new Vector4Result
                                {
                                    Value = new Vector4
                                    {
                                        X = 1,
                                        Y = 2,
                                        Z = 3,
                                        W = 1
                                    }
                                }),
                    ProtocolRequest.CommandOneofCase.InstantiatePrefabInstance =>
                        Success(
                            request,
                            response => response.InstanceIdResult =
                                new InstanceIdResult
                                {
                                    Succeeded = true,
                                    InstanceId = "go-created"
                                }),
                    ProtocolRequest.CommandOneofCase.GetViewportToolState =>
                        Success(
                            request,
                            response => response.ViewportToolStateResult =
                                new ViewportToolStateResult
                                {
                                    Operation =
                                        ViewportTransformOperation.Rotate,
                                    Space =
                                        ViewportTransformSpace.Local
                                }),
                    _ => Success(
                        request,
                        response => response.BoolResult =
                            new BoolResult { Value = true })
                };
            },
            lanes.Add);

        Assert.Equal(
            new EngineProtocolVector4(1, 2, 3, 1),
            await client.TraceViewportRayAsync(
                1,
                0.25f,
                0.75f));
        Assert.True((await client.InstantiatePrefabInstanceAsync(
            "{PREFAB}",
            string.Empty,
            null)).Succeeded);
        Assert.True(await client.FocusEditorCameraAsync(1, "go-created"));
        Assert.True(await client.SetPrefabLinkAsync("go-created", "{PREFAB}"));
        Assert.True(await client.BreakPrefabLinkAsync("go-created"));
        Assert.True(await client.SetViewportToolStateAsync(
            1,
            ViewportTransformOperation.Scale,
            ViewportTransformSpace.World));
        Assert.Equal(
            new EngineProtocolViewportToolState(
                ViewportTransformOperation.Rotate,
                ViewportTransformSpace.Local),
            await client.GetViewportToolStateAsync(1));

        Assert.Collection(
            requests,
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.TraceViewportRay,
                    request.CommandCase);
                Assert.Equal(1ul, request.TraceViewportRay.ViewportId);
                Assert.Equal(0.25f, request.TraceViewportRay.NormalizedX);
                Assert.Equal(0.75f, request.TraceViewportRay.NormalizedY);
            },
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.InstantiatePrefabInstance,
                    request.CommandCase);
                Assert.Equal("{PREFAB}", request.InstantiatePrefabInstance.FileId);
                Assert.False(request.InstantiatePrefabInstance.ApplyWorldPosition);
                Assert.Null(request.InstantiatePrefabInstance.WorldPosition);
            },
            request => Assert.Equal(
                ProtocolRequest.CommandOneofCase.FocusEditorCamera,
                request.CommandCase),
            request => Assert.Equal(
                ProtocolRequest.CommandOneofCase.SetPrefabLink,
                request.CommandCase),
            request => Assert.Equal(
                ProtocolRequest.CommandOneofCase.BreakPrefabLink,
                request.CommandCase),
            request =>
            {
                Assert.Equal(
                    ProtocolRequest.CommandOneofCase.SetViewportToolState,
                    request.CommandCase);
                Assert.Equal(
                    ViewportTransformOperation.Scale,
                    request.SetViewportToolState.Operation);
                Assert.Equal(
                    ViewportTransformSpace.World,
                    request.SetViewportToolState.Space);
            },
            request => Assert.Equal(
                ProtocolRequest.CommandOneofCase.GetViewportToolState,
                request.CommandCase));
        Assert.Equal(
            [
                EngineProtocolInvocationKind.Interactive,
                EngineProtocolInvocationKind.Request,
                EngineProtocolInvocationKind.Interactive,
                EngineProtocolInvocationKind.Request,
                EngineProtocolInvocationKind.Request,
                EngineProtocolInvocationKind.Interactive,
                EngineProtocolInvocationKind.Interactive
            ],
            lanes);
    }

    [Theory]
    [InlineData(float.NaN, 0.5f)]
    [InlineData(0.5f, float.PositiveInfinity)]
    [InlineData(-0.01f, 0.5f)]
    [InlineData(0.5f, 1.01f)]
    public async Task ViewportRay_RejectsInvalidCoordinates(
        float normalizedX,
        float normalizedY)
    {
        var invokeCount = 0;
        var client = CreateClient(request =>
        {
            invokeCount++;
            return Success(
                request,
                response => response.Vector4Result =
                    new Vector4Result { Value = new Vector4() });
        });

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            client.TraceViewportRayAsync(
                1,
                normalizedX,
                normalizedY));
        Assert.Equal(0, invokeCount);
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
            () => client.CreateModelInstanceAsync(
                "model\0id",
                "Bistro",
                "parent",
                true,
                null,
                "preferred"),
            () => client.CreateModelInstanceAsync(
                "model",
                "Bis\0tro",
                "parent",
                true,
                null,
                "preferred"),
            () => client.CreateModelInstanceAsync(
                "model",
                "Bistro",
                "parent\0id",
                true,
                null,
                "preferred"),
            () => client.CreateModelInstanceAsync(
                "model",
                "Bistro",
                "parent",
                true,
                null,
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
            () => client.InstantiatePrefabFromYamlStrictAsync(
                "yaml\0value",
                "parent"),
            () => client.InstantiatePrefabFromYamlStrictAsync(
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
        Action<ProtocolResponse> setResult,
        bool supportsStrictInstanceIds = true)
    {
        var response = new ProtocolResponse
        {
            ProtocolVersion = request.ProtocolVersion,
            RequestId = request.RequestId,
            Success = true,
            SupportsStrictInstanceIds = supportsStrictInstanceIds
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

    sealed class LocalCapabilityRecordingTransport :
        IEngineProtocolTransport,
        ILocalEngineProtocolTransport
    {
        public List<ProtocolRequest> InitializeRequests { get; } = [];
        public List<ProtocolRequest> InvokedRequests { get; } = [];

        public Task InitializeAsync(
            byte[] requestData,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            InitializeRequests.Add(
                ProtocolRequest.Parser.ParseFrom(requestData));
            return Task.CompletedTask;
        }

        public Task<byte[]> InvokeAsync(
            byte[] requestData,
            EngineProtocolInvocationKind invocationKind,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var request = ProtocolRequest.Parser.ParseFrom(requestData);
            InvokedRequests.Add(request);
            return Task.FromResult(
                Success(
                    request,
                    response =>
                    {
                        if (request.CommandCase ==
                            ProtocolRequest.CommandOneofCase.GetExitCode)
                        {
                            response.Int32Result =
                                new Int32Result { Value = 0 };
                        }
                        else
                        {
                            response.BoolResult =
                                new BoolResult { Value = true };
                        }
                    }).ToByteArray());
        }

        public Task RequestStopFallbackAsync()
            => Task.CompletedTask;

        public Task CompleteShutdownAsync(bool shutdownEngine)
            => Task.CompletedTask;

        public void Dispose()
        {
        }

        public ValueTask DisposeAsync()
            => ValueTask.CompletedTask;
    }
}
