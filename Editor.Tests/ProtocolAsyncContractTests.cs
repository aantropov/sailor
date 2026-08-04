using System.Text.RegularExpressions;

namespace Editor.Tests;

public sealed class ProtocolAsyncContractTests
{
    [Fact]
    public void ManagedProtocol_NormalRpcPathUsesAsyncIoWithoutSyncOverAsync()
    {
        var transportContract = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "EngineProtocolTransport.cs");
        var webSocketTransport = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "ClientWebSocketEngineProtocolTransport.cs");
        var localTransport = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "LocalEngineProtocolTransport.cs");
        var client = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "EngineProtocolClient.cs");

        Assert.Contains(
            "Task<byte[]> InvokeAsync(",
            transportContract,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "byte[] Invoke(",
            transportContract,
            StringComparison.Ordinal);

        var invokeStart = webSocketTransport.IndexOf(
            "Task<byte[]> InvokeAsync(",
            StringComparison.Ordinal);
        var disposeStart = webSocketTransport.IndexOf(
            "public void Dispose()",
            invokeStart,
            StringComparison.Ordinal);
        Assert.True(invokeStart >= 0);
        Assert.True(disposeStart > invokeStart);
        var normalRpcPath = webSocketTransport[invokeStart..disposeStart];

        Assert.Contains(
            "await lane.Gate.WaitAsync(",
            normalRpcPath,
            StringComparison.Ordinal);
        Assert.Contains(
            "await socket.ConnectAsync(",
            normalRpcPath,
            StringComparison.Ordinal);
        Assert.Contains(
            "await socket.SendAsync(",
            normalRpcPath,
            StringComparison.Ordinal);
        Assert.Contains(
            "await socket.ReceiveAsync(",
            normalRpcPath,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            ".GetAwaiter().GetResult()",
            normalRpcPath,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            ".Wait(",
            normalRpcPath,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            ".Result",
            normalRpcPath,
            StringComparison.Ordinal);

        Assert.Contains(
            "Task<ProtocolResponse> SendAsync(",
            client,
            StringComparison.Ordinal);
        Assert.Contains(
            "await transport.InvokeAsync(",
            client,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "transport.Invoke(",
            client,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            ".GetAwaiter().GetResult()",
            client,
            StringComparison.Ordinal);

        Assert.Contains(
            "Task InitializeAsync(",
            localTransport,
            StringComparison.Ordinal);
        var localInitialize = Slice(
            localTransport,
            "public async Task InitializeAsync(",
            "void InitializeCore(byte[] requestData)");
        Assert.Contains(
            "InitializeCore(requestData);",
            localInitialize,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Task.Run(",
            localInitialize,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Task.Yield(",
            localInitialize,
            StringComparison.Ordinal);
        Assert.Contains(
            "Task<byte[]> InvokeAsync(",
            localTransport,
            StringComparison.Ordinal);
        Assert.Contains(
            "transport.InvokeAsync(",
            localTransport,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "byte[] Invoke(",
            localTransport,
            StringComparison.Ordinal);
    }

    [Fact]
    public void EngineService_DoesNotWrapLegacySynchronousProtocolCalls()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Services",
            "EngineService.cs");
        string[] legacyRpcNames =
        [
            "Initialize",
            "Start",
            "Stop",
            "Shutdown",
            "RequestAssetReload",
            "UpdateAsset",
            "GetAssetReloadState",
            "GetExitCode",
            "IsEngineMainThreadReady",
            "IsEngineRunning",
            "GetMessages",
            "SerializeCurrentWorld",
            "SerializeEngineTypes",
            "SerializeEditorTypes",
            "SerializeWorkspaceCacheIdentity",
            "LoadEditorWorld",
            "CreateEditorWorld",
            "SetViewport",
            "SetEditorRenderTargetSize",
            "UpsertRemoteViewport",
            "DestroyRemoteViewport",
            "GetRemoteViewportState",
            "GetRemoteViewportDiagnostics",
            "RetryRemoteViewport",
            "SetRemoteViewportMacHostHandle",
            "SendRemoteViewportInput",
            "PullEditorViewportEvents",
            "GetEditorManagedMutationRevision",
            "UpdateObject",
            "ReparentObject",
            "CreateGameObject",
            "DestroyObject",
            "ResetComponentToDefaults",
            "AddComponent",
            "RemoveComponent",
            "InstantiatePrefab",
            "InstantiatePrefabFromYaml",
            "SetEditorSelection",
            "ShowMainWindow",
            "RenderPathTracedImage",
            "RequestLocalStopFallback",
            "CompleteLocalShutdownFallback"
        ];

        foreach (var rpcName in legacyRpcNames)
        {
            Assert.False(
                Regex.IsMatch(
                    source,
                    $@"protocolClient\.{rpcName}\s*\(",
                    RegexOptions.CultureInvariant),
                $"EngineService still calls synchronous protocol RPC '{rpcName}'.");
        }

        Assert.DoesNotMatch(
            @"Task\.Run\s*\(\s*\(\s*\)\s*=>\s*protocolClient\.",
            source);
        Assert.Contains(
            "await protocolClient.StartAsync(",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "await protocolClient.ShutdownAsync(",
            source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void NativeProtocol_UsesOneEditorWorkerAndBypassesItForLifecycle()
    {
        var schedulerHeader = ReadRepositoryFile(
            "Runtime",
            "Tasks",
            "Scheduler.h");
        var schedulerSource = ReadRepositoryFile(
            "Runtime",
            "Tasks",
            "Scheduler.cpp");
        var appSource = ReadRepositoryFile(
            "Runtime",
            "Sailor.cpp");
        var vulkanDeviceSource = ReadRepositoryFile(
            "Runtime",
            "GraphicsDriver",
            "Vulkan",
            "VulkanDevice.cpp");
        var protocolHeader = ReadRepositoryFile(
            "Lib",
            "EditorEngineProtocolInternal.h");
        var protocolSource = ReadRepositoryFile(
            "Lib",
            "EditorEngineProtocol.cpp");
        var webSocketSource = ReadRepositoryFile(
            "Lib",
            "EditorEngineWebSocketServer.cpp");

        Assert.Contains(
            "Editor = 4",
            schedulerHeader,
            StringComparison.Ordinal);
        Assert.Contains(
            "bool IsEditorThread() const",
            schedulerHeader,
            StringComparison.Ordinal);

        var initialize = Slice(
            schedulerSource,
            "void Scheduler::Initialize()",
            "void Scheduler::AttachCurrentThreadAsMainThread()");
        Assert.Equal(
            1,
            CountOccurrences(initialize, "\"Editor Thread\""));
        Assert.Contains(
            "EThreadType::Editor",
            initialize,
            StringComparison.Ordinal);

        var schedulerDestructor = Slice(
            schedulerSource,
            "Scheduler::~Scheduler()",
            "uint32_t Scheduler::GetNumWorkerThreads()");
        Assert.Contains(
            "NotifyWorkerThread(EThreadType::Editor, true)",
            schedulerDestructor,
            StringComparison.Ordinal);
        Assert.Contains(
            "bool Scheduler::IsEditorThread() const",
            schedulerSource,
            StringComparison.Ordinal);

        var shutdown = Slice(
            appSource,
            "void App::Shutdown()",
            "bool App::IsRendererInitialized()");
        Assert.Contains(
            "EThreadType::Editor",
            shutdown,
            StringComparison.Ordinal);
        Assert.Equal(
            2,
            CountOccurrences(
                vulkanDeviceSource,
                "GetNumRHIThreads() + 3"));

        Assert.Contains(
            "FDispatchEditorEngineProtocolOperation",
            protocolHeader,
            StringComparison.Ordinal);
        Assert.Contains(
            "m_dispatchEditorOperation",
            protocolHeader,
            StringComparison.Ordinal);
        Assert.Contains(
            "DispatchRequestOnEditorThread",
            protocolSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "DispatchEditorEngineProtocolOperationOnEditorThread",
            webSocketSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "dependencies.m_dispatchEditorOperation",
            webSocketSource,
            StringComparison.Ordinal);

        var admittedDispatch = Slice(
            protocolSource,
            "void DispatchRequestWithLifecycleAdmission(",
            "EEditorEngineTransportStatus SerializeResponse(");
        var initializeCase = Slice(
            admittedDispatch,
            "case ProtocolRequest::kInitialize:",
            "case ProtocolRequest::kStart:");
        var startCase = Slice(
            admittedDispatch,
            "case ProtocolRequest::kStart:",
            "case ProtocolRequest::kStop:");
        var stopCase = Slice(
            admittedDispatch,
            "case ProtocolRequest::kStop:",
            "case ProtocolRequest::kShutdown:");
        var shutdownCase = Slice(
            admittedDispatch,
            "case ProtocolRequest::kShutdown:",
            "case ProtocolRequest::kIsEngineRunning:");
        var livenessCase = Slice(
            admittedDispatch,
            "case ProtocolRequest::kIsEngineRunning:",
            "default:");
        Assert.DoesNotContain(
            "DispatchRequestOnEditorThread",
            initializeCase,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "DispatchRequestOnEditorThread",
            startCase,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "DispatchRequestOnEditorThread",
            stopCase,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "DispatchRequestOnEditorThread",
            shutdownCase,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "DispatchRequestOnEditorThread",
            livenessCase,
            StringComparison.Ordinal);
        Assert.Equal(
            1,
            CountOccurrences(
                admittedDispatch,
                "DispatchRequestOnEditorThread"));
    }

    static string Slice(
        string source,
        string startMarker,
        string endMarker)
    {
        var start = source.IndexOf(
            startMarker,
            StringComparison.Ordinal);
        Assert.True(
            start >= 0,
            $"Could not find source marker '{startMarker}'.");
        var end = source.IndexOf(
            endMarker,
            start + startMarker.Length,
            StringComparison.Ordinal);
        Assert.True(
            end > start,
            $"Could not find source marker '{endMarker}' after '{startMarker}'.");
        return source[start..end];
    }

    static int CountOccurrences(string source, string value)
    {
        var count = 0;
        var offset = 0;
        while ((offset = source.IndexOf(
                   value,
                   offset,
                   StringComparison.Ordinal)) >= 0)
        {
            count++;
            offset += value.Length;
        }
        return count;
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null &&
               !File.Exists(Path.Combine(
                   directory.FullName,
                   "CMakeLists.txt")))
        {
            directory = directory.Parent;
        }

        var root = directory?.FullName ??
            throw new DirectoryNotFoundException(
                "Could not locate the Sailor repository root.");
        return File.ReadAllText(
            Path.Combine([root, .. relativePath]));
    }
}
