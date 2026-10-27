using System.Collections.Concurrent;
using SailorEditor.Services;

namespace Editor.Tests;

public sealed class MaterialPreviewControllerTests
{
    [Fact]
    public async Task RapidChanges_RenderLatestSnapshotOnce_AndStopPollingWhenReady()
    {
        var transport = new PreviewTransport();
        await using var controller = Create(transport, debounce: 40);
        controller.SetContext(new object(), 1, true);
        for (var i = 0; i < 20; ++i)
            controller.Update(new($"shaderUid: shader-{i}"));

        await Until(() => controller.Presentation.Png is not null);
        Assert.Single(transport.Updates);
        Assert.Equal("shaderUid: shader-19", transport.Updates.Single().Request.Snapshot.MaterialYaml);
        Assert.Equal(20ul, controller.Presentation.ImageRevision);
        await Task.Delay(70);
        Assert.Empty(transport.Polls);
        Assert.False(controller.Presentation.IsLoading);
    }

    [Fact]
    public async Task UnchangedSnapshot_DoesNotSubmitAnotherFrame()
    {
        var transport = new PreviewTransport();
        await using var controller = Create(transport);
        var binding = new object();
        controller.SetContext(binding, 1, true);
        controller.Update(new("shaderUid: shader"));
        await Until(() => controller.Presentation.Png is not null);
        Assert.False(controller.SetContext(binding, 1, true));
        controller.Update(new("shaderUid: shader"));
        await Task.Delay(40);
        Assert.Single(transport.Updates);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task BindingOrEngineReplacement_RejectsLateImage_AndClosesOldSession(bool engineRestart)
    {
        var lateResponse = new TaskCompletionSource<MaterialPreviewResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new PreviewTransport
        {
            OnUpdate = (request, _) => request.Snapshot.MaterialYaml == "old"
                ? lateResponse.Task
                : Task.FromResult(Ready(request, [2]))
        };
        await using var controller = Create(transport);
        var binding = new object();
        controller.SetContext(binding, 1, true);
        controller.Update(new("old"));
        await Until(() => transport.Updates.Count == 1);
        var old = transport.Updates.Single().Request;

        controller.SetContext(engineRestart ? binding : new object(), engineRestart ? 2 : 1, true);
        controller.Update(new("new"));
        var newSession = controller.Presentation.SessionId;
        Assert.NotEqual(old.SessionId, newSession);
        Assert.Null(controller.Presentation.Png);
        await Task.Delay(15);
        Assert.Single(transport.Updates);
        Assert.Empty(transport.Closed);
        lateResponse.SetResult(Ready(old, [1]));
        await Until(() => controller.Presentation.Png is not null);

        Assert.Equal(new byte[] { 2 }, controller.Presentation.Png);
        Assert.Equal(newSession, controller.Presentation.SessionId);
        Assert.Contains((old.SessionId, 1L), transport.Closed);
        Assert.Equal(engineRestart ? 2 : 1, transport.Updates.Last().Generation);
    }

    [Fact]
    public async Task ErrorAfterEdit_KeepsLastGoodImage_AndStopsPolling()
    {
        var transport = new PreviewTransport
        {
            OnUpdate = (request, _) => Task.FromResult(request.Revision == 1
                ? Ready(request, [1])
                : new MaterialPreviewResponse(request.SessionId, request.Revision, PreviewState.Error, [], "Shader compilation failed"))
        };
        await using var controller = Create(transport);
        controller.SetContext(new object(), 1, true);
        controller.Update(new("good"));
        await Until(() => controller.Presentation.Png is not null);
        controller.Update(new("bad"));
        Assert.Equal(new byte[] { 1 }, controller.Presentation.Png);
        await Until(() => !controller.Presentation.IsLoading);

        Assert.Equal(new byte[] { 1 }, controller.Presentation.Png);
        Assert.Equal(1ul, controller.Presentation.ImageRevision);
        Assert.Equal("Shader compilation failed", controller.Presentation.Diagnostic);
        await Task.Delay(40);
        Assert.Empty(transport.Polls);
        Assert.Equal(2, transport.Updates.Count);
    }

    [Fact]
    public async Task NewEdit_DiscardsOlderInFlightResult_AndPassesLastImageRevisionToPoll()
    {
        var delayedPoll = new TaskCompletionSource<MaterialPreviewResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new PreviewTransport
        {
            OnUpdate = (request, _) => Task.FromResult(request.Revision == 2
                ? Pending(request)
                : Ready(request, [(byte)request.Revision])),
            OnGet = (_, _, _) => delayedPoll.Task
        };
        await using var controller = Create(transport);
        controller.SetContext(new object(), 1, true);
        controller.Update(new("first"));
        await Until(() => controller.Presentation.Png is not null);
        controller.Update(new("second"));
        await Until(() => transport.Polls.Count == 1);
        Assert.Equal(1ul, transport.Polls.Single().AfterRevision);

        controller.Update(new("third"));
        delayedPoll.SetResult(Ready(transport.Updates.Last().Request, [2]));
        await Until(() => controller.Presentation.ImageRevision == 3);
        Assert.Equal(new byte[] { 3 }, controller.Presentation.Png);
        Assert.Equal(3, transport.Updates.Count);
        Assert.Single(transport.Polls);
    }

    [Fact]
    public async Task HiddenInspector_CancelsPendingPoll_ClosesSession_AndSubmitsNothingMore()
    {
        var transport = new PreviewTransport
        {
            OnUpdate = (request, _) => Task.FromResult(Pending(request)),
            OnGet = async (_, _, cancellation) =>
            {
                await Task.Delay(Timeout.Infinite, cancellation);
                throw new InvalidOperationException("Unreachable");
            }
        };
        await using var controller = Create(transport);
        var binding = new object();
        controller.SetContext(binding, 1, true);
        controller.Update(new("material"));
        await Until(() => transport.Polls.Count == 1);
        var sessionId = controller.Presentation.SessionId;
        controller.SetContext(binding, 1, false);
        controller.Update(new("hidden edit"));
        await Until(() => transport.Closed.Count == 1);
        await Task.Delay(40);

        Assert.Single(transport.Updates);
        Assert.Single(transport.Polls);
        Assert.Contains((sessionId, 1L), transport.Closed);
        Assert.False(controller.Presentation.IsLoading);
        Assert.Null(controller.Presentation.Png);
    }

    [Fact]
    public async Task SnapshotError_CancelsQueuedRender_AndAllowsCorrectedSnapshot()
    {
        var transport = new PreviewTransport();
        await using var controller = Create(transport, debounce: 30);
        controller.SetContext(new object(), 1, true);
        controller.Update(new("good"));
        await Until(() => controller.Presentation.Png is not null);
        controller.Update(new("incomplete"));
        controller.ReportSnapshotError("Incomplete uniform");
        await Task.Delay(50);
        Assert.Single(transport.Updates);
        Assert.NotNull(controller.Presentation.Png);
        Assert.Equal("Incomplete uniform", controller.Presentation.Diagnostic);
        controller.Update(new("corrected"));
        await Until(() => controller.Presentation.ImageRevision == 3);
        Assert.Empty(controller.Presentation.Diagnostic);
    }

    [Fact]
    public async Task PendingTimeout_StopsWorkAndRetainsDiagnostic()
    {
        var transport = new PreviewTransport
        {
            OnUpdate = (request, _) => Task.FromResult(Pending(request))
        };
        await using var controller = new MaterialPreviewController(
            transport, TimeSpan.Zero, TimeSpan.FromMilliseconds(5), TimeSpan.FromMilliseconds(40));
        controller.SetContext(new object(), 1, true);
        controller.Update(new("material"));
        await Until(() => !controller.Presentation.IsLoading);
        Assert.Contains("timed out", controller.Presentation.Diagnostic);
        var count = transport.Polls.Count;
        await Task.Delay(40);
        Assert.Equal(count, transport.Polls.Count);
    }

    [Fact]
    public async Task Dispose_ClosesInFlightSession_AndRejectsLateResponse()
    {
        var late = new TaskCompletionSource<MaterialPreviewResponse>(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new PreviewTransport { OnUpdate = (_, _) => late.Task };
        var controller = Create(transport);
        controller.SetContext(new object(), 1, true);
        controller.Update(new("material"));
        await Until(() => transport.Updates.Count == 1);
        var request = transport.Updates.Single().Request;
        var disposal = controller.DisposeAsync().AsTask();
        Assert.False(disposal.IsCompleted);
        late.SetResult(Ready(request, [1]));
        await disposal.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Contains((request.SessionId, 1L), transport.Closed);
        Assert.Null(controller.Presentation.Png);
    }

    static MaterialPreviewController Create(PreviewTransport transport, int debounce = 1) => new(
        transport, TimeSpan.FromMilliseconds(debounce), TimeSpan.FromMilliseconds(5));

    static MaterialPreviewResponse Ready(MaterialPreviewRequest request, byte[] png) =>
        new(request.SessionId, request.Revision, PreviewState.Ready, png, string.Empty);

    static MaterialPreviewResponse Pending(MaterialPreviewRequest request) =>
        new(request.SessionId, request.Revision, PreviewState.Pending, [], string.Empty);

    static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        while (!condition())
            await Task.Delay(5, timeout.Token);
    }

    sealed class PreviewTransport : IMaterialPreviewTransport
    {
        public ConcurrentQueue<(MaterialPreviewRequest Request, long Generation)> Updates { get; } = new();
        public ConcurrentQueue<(ulong SessionId, ulong AfterRevision)> Polls { get; } = new();
        public ConcurrentQueue<(ulong SessionId, long Generation)> Closed { get; } = new();
        public Func<MaterialPreviewRequest, CancellationToken, Task<MaterialPreviewResponse>>? OnUpdate { get; init; }
        public Func<ulong, ulong, CancellationToken, Task<MaterialPreviewResponse>>? OnGet { get; init; }

        public Task<MaterialPreviewResponse> UpdateAsync(MaterialPreviewRequest request, long generation, CancellationToken token)
        {
            Updates.Enqueue((request, generation));
            return OnUpdate?.Invoke(request, token) ?? Task.FromResult(Ready(request, [1]));
        }

        public Task<MaterialPreviewResponse> GetAsync(ulong sessionId, ulong afterRevision, long generation, CancellationToken token)
        {
            Polls.Enqueue((sessionId, afterRevision));
            return OnGet?.Invoke(sessionId, afterRevision, token) ?? Task.FromResult(Pending(Updates.Last().Request));
        }

        public Task CloseAsync(ulong sessionId, long generation, CancellationToken token)
        {
            Closed.Enqueue((sessionId, generation));
            return Task.CompletedTask;
        }
    }
}
