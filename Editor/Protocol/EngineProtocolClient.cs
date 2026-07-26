using Google.Protobuf;
using SailorEditor.Protocol.Generated;
using System.Runtime.InteropServices;

namespace SailorEditor.Protocol;

internal delegate int EngineProtocolInvokeDelegate(
    byte[] requestData,
    uint requestSize,
    out nint responseData,
    out uint responseSize);

internal delegate void EngineProtocolFreeBufferDelegate(nint buffer);

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

internal sealed class EngineProtocolClient
{
    internal const uint ProtocolVersion = 1;
    internal const uint MaxPayloadSize = 64u * 1024u * 1024u;

    readonly EngineProtocolInvokeDelegate invoke;
    readonly EngineProtocolFreeBufferDelegate freeBuffer;
    long nextRequestId;

    public EngineProtocolClient()
        : this(
            EngineProtocolNative.SailorProtocolInvoke,
            EngineProtocolNative.SailorProtocolFreeBuffer)
    {
    }

    internal EngineProtocolClient(
        EngineProtocolInvokeDelegate invoke,
        EngineProtocolFreeBufferDelegate freeBuffer)
    {
        this.invoke = invoke ?? throw new ArgumentNullException(nameof(invoke));
        this.freeBuffer = freeBuffer ?? throw new ArgumentNullException(nameof(freeBuffer));
    }

    public void Initialize(IEnumerable<string> arguments)
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
            Send(new ProtocolRequest { Initialize = initialize }).EmptyResult,
            nameof(ProtocolRequest.Initialize));
    }

    public void Start()
        => RequireEmpty(Send(new ProtocolRequest { Start = new Empty() }), nameof(ProtocolRequest.Start));

    public void Stop()
        => RequireEmpty(Send(new ProtocolRequest { Stop = new Empty() }), nameof(ProtocolRequest.Stop));

    public void Shutdown()
        => RequireEmpty(Send(new ProtocolRequest { Shutdown = new Empty() }), nameof(ProtocolRequest.Shutdown));

    public bool RequestAssetReload()
        => ReadBool(
            Send(new ProtocolRequest { RequestAssetReload = new Empty() }),
            nameof(ProtocolRequest.RequestAssetReload));

    public EngineProtocolAssetReloadState GetAssetReloadState()
    {
        var result = RequireResult<AssetReloadStateResult>(
            Send(new ProtocolRequest { GetAssetReloadState = new Empty() }).AssetReloadStateResult,
            nameof(ProtocolRequest.GetAssetReloadState));
        return new EngineProtocolAssetReloadState(
            result.Available,
            result.RequestGeneration,
            result.CompletedGeneration,
            result.SuccessfulGeneration);
    }

    public int GetExitCode()
        => RequireResult<Int32Result>(
            Send(new ProtocolRequest { GetExitCode = new Empty() }).Int32Result,
            nameof(ProtocolRequest.GetExitCode)).Value;

    public string[] GetMessages(uint maxCount)
        => RequireResult<StringListResult>(
            Send(new ProtocolRequest
            {
                GetMessages = new CountRequest { MaxCount = maxCount }
            }).StringListResult,
            nameof(ProtocolRequest.GetMessages)).Values.ToArray();

    public string SerializeCurrentWorld()
        => ReadString(
            Send(new ProtocolRequest { SerializeCurrentWorld = new Empty() }),
            nameof(ProtocolRequest.SerializeCurrentWorld));

    public string SerializeEngineTypes()
        => ReadString(
            Send(new ProtocolRequest { SerializeEngineTypes = new Empty() }),
            nameof(ProtocolRequest.SerializeEngineTypes));

    public string SerializeEditorTypes()
        => ReadString(
            Send(new ProtocolRequest { SerializeEditorTypes = new Empty() }),
            nameof(ProtocolRequest.SerializeEditorTypes));

    public string SerializeWorkspaceCacheIdentity()
        => ReadString(
            Send(new ProtocolRequest { SerializeWorkspaceCacheIdentity = new Empty() }),
            nameof(ProtocolRequest.SerializeWorkspaceCacheIdentity));

    public bool LoadEditorWorld(string fileId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                LoadEditorWorld = new FileIdRequest
                {
                    FileId = ValidateString(fileId, nameof(fileId))
                }
            }),
            nameof(ProtocolRequest.LoadEditorWorld));

    public bool CreateEditorWorld()
        => ReadBool(
            Send(new ProtocolRequest { CreateEditorWorld = new Empty() }),
            nameof(ProtocolRequest.CreateEditorWorld));

    public void SetViewport(uint windowPosX, uint windowPosY, uint width, uint height)
        => RequireEmpty(
            Send(new ProtocolRequest
            {
                SetViewport = new ViewportRectRequest
                {
                    WindowPosX = windowPosX,
                    WindowPosY = windowPosY,
                    Width = width,
                    Height = height
                }
            }),
            nameof(ProtocolRequest.SetViewport));

    public void SetEditorRenderTargetSize(uint width, uint height)
        => RequireEmpty(
            Send(new ProtocolRequest
            {
                SetEditorRenderTargetSize = new SailorEditor.Protocol.Generated.SizeRequest
                {
                    Width = width,
                    Height = height
                }
            }),
            nameof(ProtocolRequest.SetEditorRenderTargetSize));

    public bool UpsertRemoteViewport(
        ulong viewportId,
        uint windowPosX,
        uint windowPosY,
        uint width,
        uint height,
        bool visible,
        bool focused)
        => ReadBool(
            Send(new ProtocolRequest
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
            }),
            nameof(ProtocolRequest.UpsertRemoteViewport));

    public bool DestroyRemoteViewport(ulong viewportId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                DestroyRemoteViewport = new ViewportIdRequest { ViewportId = viewportId }
            }),
            nameof(ProtocolRequest.DestroyRemoteViewport));

    public uint GetRemoteViewportState(ulong viewportId)
        => RequireResult<UInt32Result>(
            Send(new ProtocolRequest
            {
                GetRemoteViewportState = new ViewportIdRequest { ViewportId = viewportId }
            }).Uint32Result,
            nameof(ProtocolRequest.GetRemoteViewportState)).Value;

    public string GetRemoteViewportDiagnostics(ulong viewportId)
        => ReadString(
            Send(new ProtocolRequest
            {
                GetRemoteViewportDiagnostics = new ViewportIdRequest { ViewportId = viewportId }
            }),
            nameof(ProtocolRequest.GetRemoteViewportDiagnostics));

    public bool RetryRemoteViewport(ulong viewportId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                RetryRemoteViewport = new ViewportIdRequest { ViewportId = viewportId }
            }),
            nameof(ProtocolRequest.RetryRemoteViewport));

    public bool SetRemoteViewportMacHostHandle(
        ulong viewportId,
        uint hostHandleKind,
        ulong hostHandleValue)
        => ReadBool(
            Send(new ProtocolRequest
            {
                SetRemoteViewportMacHostHandle = new RemoteViewportHostRequest
                {
                    ViewportId = viewportId,
                    HostHandleKind = hostHandleKind,
                    HostHandleValue = hostHandleValue
                }
            }),
            nameof(ProtocolRequest.SetRemoteViewportMacHostHandle));

    public bool SendRemoteViewportInput(
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
        bool captured)
        => ReadBool(
            Send(new ProtocolRequest
            {
                SendRemoteViewportInput = new RemoteViewportInputRequest
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
            }),
            nameof(ProtocolRequest.SendRemoteViewportInput));

    public IReadOnlyList<ViewportEvent> PullEditorViewportEvents(uint maxCount)
        => RequireResult<ViewportEventBatchResult>(
            Send(new ProtocolRequest
            {
                PullEditorViewportEvents = new CountRequest { MaxCount = maxCount }
            }).ViewportEventBatchResult,
            nameof(ProtocolRequest.PullEditorViewportEvents)).Events.ToArray();

    public ulong GetEditorManagedMutationRevision(uint kind, string instanceId)
        => RequireResult<UInt64Result>(
            Send(new ProtocolRequest
            {
                GetEditorManagedMutationRevision = new ManagedMutationRevisionRequest
                {
                    Kind = kind,
                    InstanceId = ValidateString(instanceId, nameof(instanceId))
                }
            }).Uint64Result,
            nameof(ProtocolRequest.GetEditorManagedMutationRevision)).Value;

    public bool UpdateObject(string instanceId, string yamlChanges)
        => ReadBool(
            Send(new ProtocolRequest
            {
                UpdateObject = new UpdateObjectRequest
                {
                    InstanceId = ValidateString(instanceId, nameof(instanceId)),
                    YamlChanges = ValidateString(yamlChanges, nameof(yamlChanges))
                }
            }),
            nameof(ProtocolRequest.UpdateObject));

    public bool ReparentObject(
        string instanceId,
        string parentInstanceId,
        bool keepWorldTransform)
        => ReadBool(
            Send(new ProtocolRequest
            {
                ReparentObject = new ReparentObjectRequest
                {
                    InstanceId = ValidateString(instanceId, nameof(instanceId)),
                    ParentInstanceId = ValidateString(parentInstanceId, nameof(parentInstanceId)),
                    KeepWorldTransform = keepWorldTransform
                }
            }),
            nameof(ProtocolRequest.ReparentObject));

    public EngineProtocolCreationResult CreateGameObject(
        string parentInstanceId,
        string preferredInstanceId)
        => ReadCreation(
            Send(new ProtocolRequest
            {
                CreateGameObject = new CreateGameObjectRequest
                {
                    ParentInstanceId = ValidateString(parentInstanceId, nameof(parentInstanceId)),
                    PreferredInstanceId = ValidateString(
                        preferredInstanceId,
                        nameof(preferredInstanceId))
                }
            }),
            nameof(ProtocolRequest.CreateGameObject));

    public bool DestroyObject(string instanceId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                DestroyObject = new InstanceIdRequest
                {
                    InstanceId = ValidateString(instanceId, nameof(instanceId))
                }
            }),
            nameof(ProtocolRequest.DestroyObject));

    public bool ResetComponentToDefaults(string instanceId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                ResetComponentToDefaults = new InstanceIdRequest
                {
                    InstanceId = ValidateString(instanceId, nameof(instanceId))
                }
            }),
            nameof(ProtocolRequest.ResetComponentToDefaults));

    public EngineProtocolCreationResult AddComponent(
        string instanceId,
        string componentTypeName,
        string preferredInstanceId)
        => ReadCreation(
            Send(new ProtocolRequest
            {
                AddComponent = new AddComponentRequest
                {
                    InstanceId = ValidateString(instanceId, nameof(instanceId)),
                    ComponentTypeName = ValidateString(
                        componentTypeName,
                        nameof(componentTypeName)),
                    PreferredInstanceId = ValidateString(
                        preferredInstanceId,
                        nameof(preferredInstanceId))
                }
            }),
            nameof(ProtocolRequest.AddComponent));

    public bool RemoveComponent(string instanceId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                RemoveComponent = new InstanceIdRequest
                {
                    InstanceId = ValidateString(instanceId, nameof(instanceId))
                }
            }),
            nameof(ProtocolRequest.RemoveComponent));

    public bool InstantiatePrefab(string fileId, string parentInstanceId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                InstantiatePrefab = new InstantiatePrefabRequest
                {
                    FileId = ValidateString(fileId, nameof(fileId)),
                    ParentInstanceId = ValidateString(parentInstanceId, nameof(parentInstanceId))
                }
            }),
            nameof(ProtocolRequest.InstantiatePrefab));

    public bool InstantiatePrefabFromYaml(string prefabYaml, string parentInstanceId)
        => ReadBool(
            Send(new ProtocolRequest
            {
                InstantiatePrefabFromYaml = new InstantiatePrefabFromYamlRequest
                {
                    PrefabYaml = ValidateString(prefabYaml, nameof(prefabYaml)),
                    ParentInstanceId = ValidateString(parentInstanceId, nameof(parentInstanceId))
                }
            }),
            nameof(ProtocolRequest.InstantiatePrefabFromYaml));

    public bool SetEditorSelection(IEnumerable<string> instanceIds)
    {
        ArgumentNullException.ThrowIfNull(instanceIds);

        var selection = new SelectionRequest();
        foreach (var instanceId in instanceIds)
        {
            selection.InstanceIds.Add(ValidateString(instanceId, nameof(instanceIds)));
        }

        return ReadBool(
            Send(new ProtocolRequest { SetEditorSelection = selection }),
            nameof(ProtocolRequest.SetEditorSelection));
    }

    public void ShowMainWindow(bool show)
        => RequireEmpty(
            Send(new ProtocolRequest
            {
                ShowMainWindow = new ShowMainWindowRequest { Show = show }
            }),
            nameof(ProtocolRequest.ShowMainWindow));

    public bool RenderPathTracedImage(
        string outputPath,
        string instanceId,
        uint height,
        uint samplesPerPixel,
        uint maxBounces)
        => ReadBool(
            Send(new ProtocolRequest
            {
                RenderPathTracedImage = new RenderPathTracedImageRequest
                {
                    OutputPath = ValidateString(outputPath, nameof(outputPath)),
                    InstanceId = ValidateString(instanceId, nameof(instanceId)),
                    Height = height,
                    SamplesPerPixel = samplesPerPixel,
                    MaxBounces = maxBounces
                }
            }),
            nameof(ProtocolRequest.RenderPathTracedImage));

    internal ProtocolResponse Send(ProtocolRequest request)
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

        nint responseData = nint.Zero;
        uint responseSize = 0;
        try
        {
            var transportStatus = invoke(
                requestData,
                checked((uint)requestData.Length),
                out responseData,
                out responseSize);
            if (transportStatus != 0)
            {
                throw new EngineProtocolException(
                    $"SailorProtocolInvoke failed with transport status {transportStatus}.");
            }

            if (responseData == nint.Zero || responseSize == 0 || responseSize > MaxPayloadSize)
            {
                throw new EngineProtocolException(
                    $"SailorProtocolInvoke returned an invalid response buffer ({responseSize} bytes).");
            }

            var responseBytes = new byte[checked((int)responseSize)];
            Marshal.Copy(responseData, responseBytes, 0, responseBytes.Length);

            ProtocolResponse response;
            try
            {
                response = ProtocolResponse.Parser.ParseFrom(responseBytes);
            }
            catch (InvalidProtocolBufferException ex)
            {
                throw new EngineProtocolException(
                    "SailorProtocolInvoke returned malformed protobuf data.",
                    ex);
            }

            if (response.ProtocolVersion != ProtocolVersion)
            {
                throw new EngineProtocolException(
                    $"SailorProtocolInvoke returned protocol version {response.ProtocolVersion}; expected {ProtocolVersion}.");
            }

            if (response.RequestId != requestId)
            {
                throw new EngineProtocolException(
                    $"SailorProtocolInvoke returned request id {response.RequestId}; expected {requestId}.");
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
        finally
        {
            if (responseData != nint.Zero)
            {
                freeBuffer(responseData);
            }
        }
    }

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
}
