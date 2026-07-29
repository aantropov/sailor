using Google.Protobuf;
using SailorEditor.Protocol.Generated;

namespace SailorEditor.Protocol;

internal sealed class EngineProtocolException : Exception
{
    public EngineProtocolException(string message)
        : base(message)
    {
    }

    public EngineProtocolException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}

internal readonly record struct EngineProtocolAssetReloadState(
    bool Available,
    ulong RequestGeneration,
    ulong CompletedGeneration,
    ulong SuccessfulGeneration);

internal readonly record struct EngineProtocolCreationResult(
    bool Succeeded,
    string InstanceId);

internal sealed class EngineProtocolClient : IDisposable, IAsyncDisposable
{
    internal const uint ProtocolVersion = 1;
    internal const uint MaxPayloadSize = 64u * 1024u * 1024u;

    readonly IEngineProtocolTransport transport;
    long nextRequestId;

    public EngineProtocolClient()
        : this(new LocalEngineProtocolTransport())
    {
    }

    internal EngineProtocolClient(IEngineProtocolTransport transport)
    {
        this.transport = transport ??
            throw new ArgumentNullException(nameof(transport));
    }

    internal EngineProtocolClient(EngineProtocolInvokeAsyncDelegate invoke)
        : this(new DelegateEngineProtocolTransport(invoke))
    {
    }

    public async Task InitializeAsync(
        IEnumerable<string> arguments,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(arguments);

        var initialize = new InitializeRequest();
        var argumentIndex = 0;
        foreach (var argument in arguments)
        {
            if (argument is null)
            {
                throw new ArgumentException(
                    $"Command-line argument {argumentIndex} must not be null.",
                    nameof(arguments));
            }

            initialize.Arguments.Add(ValidateString(argument, nameof(arguments)));
            argumentIndex++;
        }

        RequireResult<Empty>(
            (await SendAsync(
                    new ProtocolRequest { Initialize = initialize },
                    cancellationToken)
                .ConfigureAwait(false)).EmptyResult,
            nameof(ProtocolRequest.Initialize));
    }

    public async Task StartAsync(
        CancellationToken cancellationToken = default)
        => RequireEmpty(
            await SendAsync(
                new ProtocolRequest { Start = new Empty() },
                cancellationToken).ConfigureAwait(false),
            nameof(ProtocolRequest.Start));

    public async Task StopAsync(
        CancellationToken cancellationToken = default)
        => RequireEmpty(
            await SendAsync(
                    new ProtocolRequest { Stop = new Empty() },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.Stop));

    public async Task ShutdownAsync(
        CancellationToken cancellationToken = default)
    {
        RequireEmpty(
            await SendAsync(
                    new ProtocolRequest { Shutdown = new Empty() },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.Shutdown));
        if (transport is ILocalEngineProtocolTransport localTransport)
        {
            await localTransport.CompleteShutdownAsync(
                    shutdownEngine: false)
                .ConfigureAwait(false);
        }
    }

    internal Task RequestLocalStopFallbackAsync()
    {
        if (transport is ILocalEngineProtocolTransport localTransport)
        {
            return localTransport.RequestStopFallbackAsync();
        }
        return Task.CompletedTask;
    }

    internal Task CompleteLocalShutdownFallbackAsync()
    {
        if (transport is ILocalEngineProtocolTransport localTransport)
        {
            return localTransport.CompleteShutdownAsync(
                shutdownEngine: true);
        }
        return Task.CompletedTask;
    }

    public async Task<bool> RequestAssetReloadAsync(
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                new ProtocolRequest { RequestAssetReload = new Empty() },
                cancellationToken).ConfigureAwait(false),
            nameof(ProtocolRequest.RequestAssetReload));

    public async Task<EngineProtocolAssetReloadState> GetAssetReloadStateAsync(
        CancellationToken cancellationToken = default)
    {
        var result = RequireResult<AssetReloadStateResult>(
            (await SendAsync(
                    new ProtocolRequest { GetAssetReloadState = new Empty() },
                    cancellationToken)
                .ConfigureAwait(false)).AssetReloadStateResult,
            nameof(ProtocolRequest.GetAssetReloadState));
        return new EngineProtocolAssetReloadState(
            result.Available,
            result.RequestGeneration,
            result.CompletedGeneration,
            result.SuccessfulGeneration);
    }

    public async Task<int> GetExitCodeAsync(
        CancellationToken cancellationToken = default)
        => RequireResult<Int32Result>(
            (await SendAsync(
                    new ProtocolRequest { GetExitCode = new Empty() },
                    cancellationToken)
                .ConfigureAwait(false)).Int32Result,
            nameof(ProtocolRequest.GetExitCode)).Value;

    public async Task<bool> IsEngineMainThreadReadyAsync(
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        IsEngineMainThreadReady = new Empty()
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.IsEngineMainThreadReady));

    public async Task<bool> IsEngineRunningAsync(
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                new ProtocolRequest { IsEngineRunning = new Empty() },
                cancellationToken).ConfigureAwait(false),
            nameof(ProtocolRequest.IsEngineRunning));

    public async Task<string[]> GetMessagesAsync(
        uint maxCount,
        CancellationToken cancellationToken = default)
        => RequireResult<StringListResult>(
            (await SendAsync(
                    new ProtocolRequest
                    {
                        GetMessages =
                            new CountRequest { MaxCount = maxCount }
                    },
                    cancellationToken)
                .ConfigureAwait(false)).StringListResult,
            nameof(ProtocolRequest.GetMessages)).Values.ToArray();

    public async Task<string> SerializeCurrentWorldAsync(
        CancellationToken cancellationToken = default)
        => ReadString(
            await SendAsync(
                new ProtocolRequest { SerializeCurrentWorld = new Empty() },
                cancellationToken).ConfigureAwait(false),
            nameof(ProtocolRequest.SerializeCurrentWorld));

    public async Task<string> SerializeEngineTypesAsync(
        CancellationToken cancellationToken = default)
        => ReadString(
            await SendAsync(
                new ProtocolRequest { SerializeEngineTypes = new Empty() },
                cancellationToken).ConfigureAwait(false),
            nameof(ProtocolRequest.SerializeEngineTypes));

    public async Task<string> SerializeEditorTypesAsync(
        CancellationToken cancellationToken = default)
        => ReadString(
            await SendAsync(
                new ProtocolRequest { SerializeEditorTypes = new Empty() },
                cancellationToken).ConfigureAwait(false),
            nameof(ProtocolRequest.SerializeEditorTypes));

    public async Task<string> SerializeWorkspaceCacheIdentityAsync(
        CancellationToken cancellationToken = default)
        => ReadString(
            await SendAsync(
                new ProtocolRequest { SerializeWorkspaceCacheIdentity = new Empty() },
                cancellationToken).ConfigureAwait(false),
            nameof(ProtocolRequest.SerializeWorkspaceCacheIdentity));

    public async Task<bool> LoadEditorWorldAsync(
        string fileId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        LoadEditorWorld = new FileIdRequest
                        {
                            FileId = ValidateString(fileId, nameof(fileId))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.LoadEditorWorld));

    public async Task<bool> CreateEditorWorldAsync(
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        CreateEditorWorld = new Empty()
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.CreateEditorWorld));

    public async Task SetViewportAsync(
        uint windowPosX,
        uint windowPosY,
        uint width,
        uint height,
        CancellationToken cancellationToken = default)
        => RequireEmpty(
            await SendAsync(
                    new ProtocolRequest
                    {
                        SetViewport = new ViewportRectRequest
                        {
                            WindowPosX = windowPosX,
                            WindowPosY = windowPosY,
                            Width = width,
                            Height = height
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.SetViewport));

    public async Task SetEditorRenderTargetSizeAsync(
        uint width,
        uint height,
        CancellationToken cancellationToken = default)
        => RequireEmpty(
            await SendAsync(
                    new ProtocolRequest
                    {
                        SetEditorRenderTargetSize =
                            new SailorEditor.Protocol.Generated.SizeRequest
                            {
                                Width = width,
                                Height = height
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.SetEditorRenderTargetSize));

    public async Task<bool> UpsertRemoteViewportAsync(
        ulong viewportId,
        uint windowPosX,
        uint windowPosY,
        uint width,
        uint height,
        bool visible,
        bool focused,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        UpsertRemoteViewport = new RemoteViewportRequest
                        {
                            ViewportId = viewportId,
                            WindowPosX = windowPosX,
                            WindowPosY = windowPosY,
                            Width = width,
                            Height = height,
                            Visible = visible,
                            Focused = focused
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.UpsertRemoteViewport));

    public async Task<bool> DestroyRemoteViewportAsync(
        ulong viewportId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        DestroyRemoteViewport =
                            new ViewportIdRequest
                            {
                                ViewportId = viewportId
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.DestroyRemoteViewport));

    public async Task<uint> GetRemoteViewportStateAsync(
        ulong viewportId,
        CancellationToken cancellationToken = default)
        => RequireResult<UInt32Result>(
            (await SendAsync(
                    new ProtocolRequest
                    {
                        GetRemoteViewportState =
                            new ViewportIdRequest
                            {
                                ViewportId = viewportId
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false)).Uint32Result,
            nameof(ProtocolRequest.GetRemoteViewportState)).Value;

    public async Task<string> GetRemoteViewportDiagnosticsAsync(
        ulong viewportId,
        CancellationToken cancellationToken = default)
        => ReadString(
            await SendAsync(
                    new ProtocolRequest
                    {
                        GetRemoteViewportDiagnostics =
                            new ViewportIdRequest
                            {
                                ViewportId = viewportId
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.GetRemoteViewportDiagnostics));

    public async Task<bool> RetryRemoteViewportAsync(
        ulong viewportId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        RetryRemoteViewport =
                            new ViewportIdRequest
                            {
                                ViewportId = viewportId
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.RetryRemoteViewport));

    public async Task<bool> SetRemoteViewportMacHostHandleAsync(
        ulong viewportId,
        uint hostHandleKind,
        ulong hostHandleValue,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        SetRemoteViewportMacHostHandle =
                            new RemoteViewportHostRequest
                            {
                                ViewportId = viewportId,
                                HostHandleKind = hostHandleKind,
                                HostHandleValue = hostHandleValue
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.SetRemoteViewportMacHostHandle));

    public async Task<bool> SendRemoteViewportInputAsync(
        ulong viewportId,
        uint kind,
        float pointerX,
        float pointerY,
        float wheelDeltaX,
        float wheelDeltaY,
        uint keyCode,
        uint button,
        uint modifiers,
        bool pressed,
        bool focused,
        bool captured,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        SendRemoteViewportInput =
                            new RemoteViewportInputRequest
                            {
                                ViewportId = viewportId,
                                Kind = kind,
                                PointerX = pointerX,
                                PointerY = pointerY,
                                WheelDeltaX = wheelDeltaX,
                                WheelDeltaY = wheelDeltaY,
                                KeyCode = keyCode,
                                Button = button,
                                Modifiers = modifiers,
                                Pressed = pressed,
                                Focused = focused,
                                Captured = captured
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.SendRemoteViewportInput));

    public async Task<IReadOnlyList<ViewportEvent>> PullEditorViewportEventsAsync(
        uint maxCount,
        CancellationToken cancellationToken = default)
        => RequireResult<ViewportEventBatchResult>(
            (await SendAsync(
                    new ProtocolRequest
                    {
                        PullEditorViewportEvents =
                            new CountRequest { MaxCount = maxCount }
                    },
                    cancellationToken)
                .ConfigureAwait(false)).ViewportEventBatchResult,
            nameof(ProtocolRequest.PullEditorViewportEvents)).Events.ToArray();

    public async Task<ulong> GetEditorManagedMutationRevisionAsync(
        uint kind,
        string instanceId,
        CancellationToken cancellationToken = default)
        => RequireResult<UInt64Result>(
            (await SendAsync(
                    new ProtocolRequest
                    {
                        GetEditorManagedMutationRevision =
                            new ManagedMutationRevisionRequest
                            {
                                Kind = kind,
                                InstanceId = ValidateString(
                                    instanceId,
                                    nameof(instanceId))
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false)).Uint64Result,
            nameof(ProtocolRequest.GetEditorManagedMutationRevision)).Value;

    public async Task<bool> UpdateObjectAsync(
        string instanceId,
        string yamlChanges,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        UpdateObject = new UpdateObjectRequest
                        {
                            InstanceId = ValidateString(
                                instanceId,
                                nameof(instanceId)),
                            YamlChanges = ValidateString(
                                yamlChanges,
                                nameof(yamlChanges))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.UpdateObject));

    public async Task<bool> ReparentObjectAsync(
        string instanceId,
        string parentInstanceId,
        bool keepWorldTransform,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        ReparentObject = new ReparentObjectRequest
                        {
                            InstanceId = ValidateString(
                                instanceId,
                                nameof(instanceId)),
                            ParentInstanceId = ValidateString(
                                parentInstanceId,
                                nameof(parentInstanceId)),
                            KeepWorldTransform = keepWorldTransform
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.ReparentObject));

    public async Task<EngineProtocolCreationResult> CreateGameObjectAsync(
        string parentInstanceId,
        string preferredInstanceId,
        CancellationToken cancellationToken = default)
        => ReadCreation(
            await SendAsync(
                    new ProtocolRequest
                    {
                        CreateGameObject = new CreateGameObjectRequest
                        {
                            ParentInstanceId = ValidateString(
                                parentInstanceId,
                                nameof(parentInstanceId)),
                            PreferredInstanceId = ValidateString(
                                preferredInstanceId,
                                nameof(preferredInstanceId))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.CreateGameObject));

    public async Task<bool> DestroyObjectAsync(
        string instanceId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        DestroyObject = new InstanceIdRequest
                        {
                            InstanceId = ValidateString(
                                instanceId,
                                nameof(instanceId))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.DestroyObject));

    public async Task<bool> ResetComponentToDefaultsAsync(
        string instanceId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        ResetComponentToDefaults = new InstanceIdRequest
                        {
                            InstanceId = ValidateString(
                                instanceId,
                                nameof(instanceId))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.ResetComponentToDefaults));

    public async Task<EngineProtocolCreationResult> AddComponentAsync(
        string instanceId,
        string componentTypeName,
        string preferredInstanceId,
        CancellationToken cancellationToken = default)
        => ReadCreation(
            await SendAsync(
                    new ProtocolRequest
                    {
                        AddComponent = new AddComponentRequest
                        {
                            InstanceId = ValidateString(
                                instanceId,
                                nameof(instanceId)),
                            ComponentTypeName = ValidateString(
                                componentTypeName,
                                nameof(componentTypeName)),
                            PreferredInstanceId = ValidateString(
                                preferredInstanceId,
                                nameof(preferredInstanceId))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.AddComponent));

    public async Task<bool> RemoveComponentAsync(
        string instanceId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        RemoveComponent = new InstanceIdRequest
                        {
                            InstanceId = ValidateString(
                                instanceId,
                                nameof(instanceId))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.RemoveComponent));

    public async Task<bool> InstantiatePrefabAsync(
        string fileId,
        string parentInstanceId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        InstantiatePrefab = new InstantiatePrefabRequest
                        {
                            FileId = ValidateString(
                                fileId,
                                nameof(fileId)),
                            ParentInstanceId = ValidateString(
                                parentInstanceId,
                                nameof(parentInstanceId))
                        }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.InstantiatePrefab));

    public async Task<bool> InstantiatePrefabFromYamlAsync(
        string prefabYaml,
        string parentInstanceId,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        InstantiatePrefabFromYaml =
                            new InstantiatePrefabFromYamlRequest
                            {
                                PrefabYaml = ValidateString(
                                    prefabYaml,
                                    nameof(prefabYaml)),
                                ParentInstanceId = ValidateString(
                                    parentInstanceId,
                                    nameof(parentInstanceId))
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.InstantiatePrefabFromYaml));

    public async Task<bool> SetEditorSelectionAsync(
        IEnumerable<string> instanceIds,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(instanceIds);

        var selection = new SelectionRequest();
        foreach (var instanceId in instanceIds)
        {
            selection.InstanceIds.Add(ValidateString(instanceId, nameof(instanceIds)));
        }

        return ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        SetEditorSelection = selection
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.SetEditorSelection));
    }

    public async Task ShowMainWindowAsync(
        bool show,
        CancellationToken cancellationToken = default)
        => RequireEmpty(
            await SendAsync(
                    new ProtocolRequest
                    {
                        ShowMainWindow =
                            new ShowMainWindowRequest { Show = show }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.ShowMainWindow));

    public async Task<bool> RenderPathTracedImageAsync(
        string outputPath,
        string instanceId,
        uint height,
        uint samplesPerPixel,
        uint maxBounces,
        CancellationToken cancellationToken = default)
        => ReadBool(
            await SendAsync(
                    new ProtocolRequest
                    {
                        RenderPathTracedImage =
                            new RenderPathTracedImageRequest
                            {
                                OutputPath = ValidateString(
                                    outputPath,
                                    nameof(outputPath)),
                                InstanceId = ValidateString(
                                    instanceId,
                                    nameof(instanceId)),
                                Height = height,
                                SamplesPerPixel = samplesPerPixel,
                                MaxBounces = maxBounces
                            }
                    },
                    cancellationToken)
                .ConfigureAwait(false),
            nameof(ProtocolRequest.RenderPathTracedImage));

    internal async Task<ProtocolResponse> SendAsync(
        ProtocolRequest request,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.CommandCase == ProtocolRequest.CommandOneofCase.None)
        {
            throw new ArgumentException(
                "The protocol request must contain exactly one command.",
                nameof(request));
        }

        var requestId = NextRequestId();
        request.ProtocolVersion = ProtocolVersion;
        request.RequestId = requestId;

        var requestData = request.ToByteArray();
        if (requestData.Length == 0 || requestData.Length > MaxPayloadSize)
        {
            throw new EngineProtocolException(
                $"The protocol request size {requestData.Length} is outside the supported range.");
        }

        ProtocolResponse response;
        if (request.CommandCase == ProtocolRequest.CommandOneofCase.Initialize &&
            transport is ILocalEngineProtocolTransport localTransport)
        {
            await localTransport.InitializeAsync(
                    requestData,
                    cancellationToken)
                .ConfigureAwait(false);
            response = new ProtocolResponse
            {
                ProtocolVersion = ProtocolVersion,
                RequestId = requestId,
                Success = true,
                EmptyResult = new Empty()
            };
        }
        else
        {
            var responseBytes = await transport.InvokeAsync(
                    requestData,
                    GetInvocationKind(request.CommandCase),
                    cancellationToken)
                .ConfigureAwait(false);
            if (responseBytes.Length == 0 ||
                responseBytes.Length > MaxPayloadSize)
            {
                throw new EngineProtocolException(
                    $"Engine protocol transport returned an invalid response " +
                    $"({responseBytes.Length} bytes).");
            }

            try
            {
                response = ProtocolResponse.Parser.ParseFrom(responseBytes);
            }
            catch (InvalidProtocolBufferException ex)
            {
                throw new EngineProtocolException(
                    "Engine protocol transport returned malformed protobuf data.",
                    ex);
            }
        }

        if (response.ProtocolVersion != ProtocolVersion)
        {
            throw new EngineProtocolException(
                $"Engine protocol transport returned protocol version " +
                $"{response.ProtocolVersion}; expected {ProtocolVersion}.");
        }

        if (response.RequestId != requestId)
        {
            throw new EngineProtocolException(
                $"Engine protocol transport returned request id " +
                $"{response.RequestId}; expected {requestId}.");
        }

        if (!response.Success)
        {
            var error = string.IsNullOrWhiteSpace(response.Error)
                ? "The engine rejected the protocol request."
                : response.Error;
            throw new EngineProtocolException(error);
        }

        return response;
    }

    static EngineProtocolInvocationKind GetInvocationKind(
        ProtocolRequest.CommandOneofCase command)
        => command switch
        {
            ProtocolRequest.CommandOneofCase.Start =>
                EngineProtocolInvocationKind.Lifecycle,
            ProtocolRequest.CommandOneofCase.RenderPathTracedImage =>
                EngineProtocolInvocationKind.Background,
            ProtocolRequest.CommandOneofCase.Stop or
                ProtocolRequest.CommandOneofCase.Shutdown or
                ProtocolRequest.CommandOneofCase.IsEngineRunning =>
                EngineProtocolInvocationKind.Lifecycle,
            ProtocolRequest.CommandOneofCase.SetViewport or
                ProtocolRequest.CommandOneofCase.SetEditorRenderTargetSize or
                ProtocolRequest.CommandOneofCase.UpsertRemoteViewport or
                ProtocolRequest.CommandOneofCase.DestroyRemoteViewport or
                ProtocolRequest.CommandOneofCase.GetRemoteViewportState or
                ProtocolRequest.CommandOneofCase.GetRemoteViewportDiagnostics or
                ProtocolRequest.CommandOneofCase.RetryRemoteViewport or
                ProtocolRequest.CommandOneofCase.SetRemoteViewportMacHostHandle or
                ProtocolRequest.CommandOneofCase.SendRemoteViewportInput or
                ProtocolRequest.CommandOneofCase.PullEditorViewportEvents or
                ProtocolRequest.CommandOneofCase.ShowMainWindow or
                ProtocolRequest.CommandOneofCase.IsEngineMainThreadReady =>
                EngineProtocolInvocationKind.Interactive,
            _ => EngineProtocolInvocationKind.Request
        };

    ulong NextRequestId()
    {
        var requestId = Interlocked.Increment(ref nextRequestId);
        if (requestId <= 0)
        {
            throw new EngineProtocolException("The protocol request id space was exhausted.");
        }

        return checked((ulong)requestId);
    }

    static string ValidateString(string? value, string parameterName)
    {
        var normalizedValue = value ?? string.Empty;
        if (normalizedValue.Contains('\0', StringComparison.Ordinal))
        {
            throw new ArgumentException(
                "Protocol string values must not contain an embedded null character.",
                parameterName);
        }

        return normalizedValue;
    }

    static void RequireEmpty(ProtocolResponse response, string commandName)
        => RequireResult<Empty>(response.EmptyResult, commandName);

    static bool ReadBool(ProtocolResponse response, string commandName)
        => RequireResult<BoolResult>(response.BoolResult, commandName).Value;

    static string ReadString(ProtocolResponse response, string commandName)
    {
        var result = RequireResult<StringResult>(response.StringResult, commandName);
        return result.HasValue ? result.Value : string.Empty;
    }

    static EngineProtocolCreationResult ReadCreation(
        ProtocolResponse response,
        string commandName)
    {
        var result = RequireResult<InstanceIdResult>(response.InstanceIdResult, commandName);
        return new EngineProtocolCreationResult(result.Succeeded, result.InstanceId);
    }

    static T RequireResult<T>(T? result, string commandName)
        where T : class
        => result ?? throw new EngineProtocolException(
            $"The protocol response for '{commandName}' has an unexpected result type.");

    public void Dispose() => transport.Dispose();

    public ValueTask DisposeAsync() => transport.DisposeAsync();
}
