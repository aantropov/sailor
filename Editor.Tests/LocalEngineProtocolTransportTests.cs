using SailorEditor.Protocol;

namespace Editor.Tests;

[CollectionDefinition(
    "LocalEngineProtocolTransport",
    DisableParallelization = true)]
public sealed class LocalEngineProtocolTransportCollection
{
}

[Collection("LocalEngineProtocolTransport")]
public sealed class LocalEngineProtocolTransportTests
{
    static readonly TimeSpan TestTimeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task Dispose_DuringNativeStart_ReturnsBeforeStartAndCleansHost()
    {
        var startEntered = NewSignal();
        using var startRelease = new ManualResetEventSlim();
        var bridge = new FakeNativeBridge
        {
            Start = () =>
            {
                startEntered.TrySetResult();
                WaitForRelease(startRelease, "native start");
                return 0;
            }
        };
        var factoryCalls = 0;
        var transport = new LocalEngineProtocolTransport(
            bridge,
            (_, _) =>
            {
                Interlocked.Increment(ref factoryCalls);
                return new RecordingTransport();
            });

        var initialization = Task.Run(() => transport.Initialize([1]));
        await startEntered.Task.WaitAsync(TestTimeout);

        var disposal = Task.Run(transport.Dispose);
        var disposedBeforeStartReleased =
            await CompletesWithinTimeout(disposal);
        startRelease.Set();

        await disposal.WaitAsync(TestTimeout);
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => initialization.WaitAsync(TestTimeout));
        await bridge.StopCompleted.Task.WaitAsync(TestTimeout);

        Assert.True(disposedBeforeStartReleased);
        Assert.Equal(0, Volatile.Read(ref factoryCalls));
        Assert.Equal(1, bridge.RequestStopCount);
        Assert.Equal(1, bridge.StopCount);
        Assert.True(bridge.LastShutdownEngine);
        await AssertHostCanBeReused();
    }

    [Fact]
    public async Task Dispose_DuringTransportCreation_CleansHostAndCandidate()
    {
        var factoryEntered = NewSignal();
        using var factoryRelease = new ManualResetEventSlim();
        var bridge = new FakeNativeBridge();
        var candidateTransport = new RecordingTransport();
        var transport = new LocalEngineProtocolTransport(
            bridge,
            (_, _) =>
            {
                factoryEntered.TrySetResult();
                WaitForRelease(factoryRelease, "transport factory");
                return candidateTransport;
            });

        var initialization = Task.Run(() => transport.Initialize([1]));
        await factoryEntered.Task.WaitAsync(TestTimeout);

        var disposal = Task.Run(transport.Dispose);
        var disposedBeforeFactoryReleased =
            await CompletesWithinTimeout(disposal);
        await bridge.StopCompleted.Task.WaitAsync(TestTimeout);
        factoryRelease.Set();

        await disposal.WaitAsync(TestTimeout);
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => initialization.WaitAsync(TestTimeout));

        Assert.True(disposedBeforeFactoryReleased);
        Assert.Equal(1, candidateTransport.DisposeCount);
        Assert.Equal(1, bridge.RequestStopCount);
        Assert.Equal(1, bridge.StopCount);
        Assert.True(bridge.LastShutdownEngine);
        await AssertHostCanBeReused();
    }

    [Fact]
    public async Task Dispose_ReturnsWhileNativeShutdownIsBlocked()
    {
        var stopEntered = NewSignal();
        using var stopRelease = new ManualResetEventSlim();
        var bridge = new FakeNativeBridge
        {
            Stop = _ =>
            {
                stopEntered.TrySetResult();
                WaitForRelease(stopRelease, "native shutdown");
            }
        };
        var candidateTransport = new RecordingTransport();
        var transport = new LocalEngineProtocolTransport(
            bridge,
            (_, _) => candidateTransport);
        transport.Initialize([1]);

        var disposal = Task.Run(transport.Dispose);
        await stopEntered.Task.WaitAsync(TestTimeout);
        var disposedBeforeShutdownReleased =
            await CompletesWithinTimeout(disposal);
        var competingBridge = new FakeNativeBridge();
        using var competingTransport = new LocalEngineProtocolTransport(
            competingBridge,
            (_, _) => new RecordingTransport());
        var competingInitialization = Task.Run(() =>
            Record.Exception(() =>
                competingTransport.Initialize([2])));
        var competingInitializeRejectedBeforeShutdownReleased =
            await CompletesWithinTimeout(competingInitialization);
        stopRelease.Set();

        await disposal.WaitAsync(TestTimeout);
        await bridge.StopCompleted.Task.WaitAsync(TestTimeout);
        var competingException =
            await competingInitialization.WaitAsync(TestTimeout);
        if (competingException is null)
        {
            competingTransport.CompleteShutdown(shutdownEngine: true);
        }

        Assert.True(disposedBeforeShutdownReleased);
        Assert.True(
            competingInitializeRejectedBeforeShutdownReleased);
        Assert.IsType<EngineProtocolException>(competingException);
        Assert.Equal(0, competingBridge.StartCount);
        Assert.Equal(1, candidateTransport.DisposeCount);
        Assert.Equal(1, bridge.RequestStopCount);
        Assert.Equal(1, bridge.StopCount);
        Assert.True(bridge.LastShutdownEngine);
        await AssertHostCanBeReused();
    }

    [Fact]
    public async Task InFlightStopRequest_BlocksHostHandoffUntilTeardown()
    {
        var requestStopEntered = NewSignal();
        using var requestStopRelease = new ManualResetEventSlim();
        var requestInFlight = 0;
        var stopObservedRequestInFlight = false;
        var bridge = new FakeNativeBridge
        {
            RequestStop = () =>
            {
                Volatile.Write(ref requestInFlight, 1);
                try
                {
                    requestStopEntered.TrySetResult();
                    WaitForRelease(
                        requestStopRelease,
                        "native stop request");
                }
                finally
                {
                    Volatile.Write(ref requestInFlight, 0);
                }
            },
            Stop = _ => stopObservedRequestInFlight =
                Volatile.Read(ref requestInFlight) != 0
        };
        var transportDisposed = NewSignal();
        var candidateTransport = new RecordingTransport(
            () => transportDisposed.TrySetResult());
        using var transport = new LocalEngineProtocolTransport(
            bridge,
            (_, _) => candidateTransport);
        transport.Initialize([1]);

        var stopRequest = Task.Run(transport.RequestStopFallback);
        await requestStopEntered.Task.WaitAsync(TestTimeout);
        var shutdown = Task.Run(
            () => transport.CompleteShutdown(shutdownEngine: true));
        await transportDisposed.Task.WaitAsync(TestTimeout);
        var shutdownCompletedBeforeRequestReleased =
            await CompletesWithinTimeout(
                shutdown,
                TimeSpan.FromMilliseconds(250));
        var competingBridge = new FakeNativeBridge();
        using var competingTransport = new LocalEngineProtocolTransport(
            competingBridge,
            (_, _) => new RecordingTransport());
        var competingInitialization = Task.Run(() =>
            Record.Exception(() =>
                competingTransport.Initialize([2])));
        var competingException =
            await competingInitialization.WaitAsync(TestTimeout);
        requestStopRelease.Set();

        await stopRequest.WaitAsync(TestTimeout);
        await shutdown.WaitAsync(TestTimeout);
        if (competingException is null)
        {
            competingTransport.CompleteShutdown(shutdownEngine: true);
        }

        Assert.False(shutdownCompletedBeforeRequestReleased);
        Assert.IsType<EngineProtocolException>(competingException);
        Assert.Equal(0, competingBridge.StartCount);
        Assert.False(stopObservedRequestInFlight);
        Assert.Equal(1, bridge.RequestStopCount);
        Assert.Equal(1, bridge.StopCount);
        await AssertHostCanBeReused();
    }

    [Fact]
    public async Task Dispose_ReleasesHostWhenNativeShutdownThrows()
    {
        var bridge = new FakeNativeBridge
        {
            Stop = _ => throw new InvalidOperationException(
                "shutdown failed")
        };
        var candidateTransport = new RecordingTransport();
        var transport = new LocalEngineProtocolTransport(
            bridge,
            (_, _) => candidateTransport);
        transport.Initialize([1]);

        transport.Dispose();
        await bridge.StopCompleted.Task.WaitAsync(TestTimeout);

        Assert.Equal(1, candidateTransport.DisposeCount);
        Assert.Equal(1, bridge.RequestStopCount);
        Assert.Equal(1, bridge.StopCount);
        await AssertHostCanBeReused();
    }

    [Fact]
    public async Task TransportFactoryFailure_CleansHostAndAllowsRetry()
    {
        var bridge = new FakeNativeBridge();
        var transport = new LocalEngineProtocolTransport(
            bridge,
            (_, _) => throw new InvalidOperationException(
                "factory failed"));

        var exception = Assert.Throws<InvalidOperationException>(
            () => transport.Initialize([1]));

        Assert.Equal("factory failed", exception.Message);
        Assert.Equal(0, bridge.RequestStopCount);
        Assert.Equal(1, bridge.StopCount);
        Assert.True(bridge.LastShutdownEngine);
        transport.Dispose();
        await AssertHostCanBeReused();
    }

    static TaskCompletionSource NewSignal()
        => new(TaskCreationOptions.RunContinuationsAsynchronously);

    static async Task<bool> CompletesWithinTimeout(Task task)
        => await CompletesWithinTimeout(task, TestTimeout);

    static async Task<bool> CompletesWithinTimeout(
        Task task,
        TimeSpan timeout)
        => ReferenceEquals(
            await Task.WhenAny(task, Task.Delay(timeout)),
            task);

    static void WaitForRelease(
        ManualResetEventSlim release,
        string operation)
    {
        if (!release.Wait(TestTimeout))
        {
            throw new TimeoutException(
                $"Timed out waiting to release {operation}.");
        }
    }

    static async Task AssertHostCanBeReused()
    {
        var bridge = new FakeNativeBridge();
        var candidateTransport = new RecordingTransport();
        using var transport = new LocalEngineProtocolTransport(
            bridge,
            (_, _) => candidateTransport);
        var deadline = DateTime.UtcNow + TestTimeout;

        while (true)
        {
            try
            {
                transport.Initialize([2]);
                break;
            }
            catch (EngineProtocolException) when (
                DateTime.UtcNow < deadline)
            {
                await Task.Yield();
            }
        }

        transport.CompleteShutdown(shutdownEngine: true);
        Assert.Equal(1, candidateTransport.DisposeCount);
        Assert.Equal(1, bridge.StopCount);
        Assert.True(bridge.LastShutdownEngine);
    }

    sealed class FakeNativeBridge : ILocalEngineProtocolNativeBridge
    {
        int requestStopCount;
        int startCount;
        int stopCount;

        public Func<int> Start { get; init; } = static () => 0;
        public Action RequestStop { get; init; } = static () => { };
        public Action<bool> Stop { get; init; } = static _ => { };
        public TaskCompletionSource<bool> StopCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int RequestStopCount =>
            Volatile.Read(ref requestStopCount);
        public int StartCount => Volatile.Read(ref startCount);
        public int StopCount => Volatile.Read(ref stopCount);
        public bool LastShutdownEngine { get; private set; }

        public int StartLocalHost(
            byte[] requestData,
            ushort port,
            string authorizationToken)
        {
            Interlocked.Increment(ref startCount);
            return Start();
        }

        public void RequestLocalHostStop()
        {
            Interlocked.Increment(ref requestStopCount);
            RequestStop();
        }

        public void StopLocalHost(bool shutdownEngine)
        {
            LastShutdownEngine = shutdownEngine;
            Interlocked.Increment(ref stopCount);
            try
            {
                Stop(shutdownEngine);
            }
            finally
            {
                StopCompleted.TrySetResult(shutdownEngine);
            }
        }
    }

    sealed class RecordingTransport(Action? onDispose = null) :
        IEngineProtocolTransport
    {
        int disposeCount;

        public int DisposeCount => Volatile.Read(ref disposeCount);

        public byte[] Invoke(
            byte[] requestData,
            EngineProtocolInvocationKind invocationKind,
            CancellationToken cancellationToken = default)
            => [];

        public void Dispose()
        {
            Interlocked.Increment(ref disposeCount);
            onDispose?.Invoke();
        }
    }
}
