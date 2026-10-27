using System.Collections.Concurrent;
using SailorEditor.Services;

namespace Editor.Tests;

public sealed class MaterialThumbnailCacheTests
{
    [Fact]
    public async Task SameDraft_DeduplicatesRequests_CachesPixels_AndClosesSession()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new Transport(async (request, token) =>
        {
            await release.Task.WaitAsync(token);
            return Ready(request);
        });
        using var cache = new MaterialThumbnailCache(transport);
        var key = Key();
        var first = cache.GetAsync(key, "material");
        var second = cache.GetAsync(key, "material");
        await Until(() => transport.Updates.Count == 1);
        release.SetResult();
        Assert.Equal(await first, await second);
        Assert.Equal(await first, await cache.GetAsync(key, "material"));
        var request = Assert.Single(transport.Updates);
        Assert.Equal(128u, request.Snapshot.Width);
        Assert.Equal(128u, request.Snapshot.Height);
        Assert.Equal(PreviewPrimitive.Sphere, request.Snapshot.Primitive);
        Assert.Equal(request.SessionId, Assert.Single(transport.Closed));
    }

    [Fact]
    public async Task OneFieldUnloads_SharedRequestStillCompletesForOtherField()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new Transport(async (request, token) =>
        {
            await release.Task.WaitAsync(token);
            return Ready(request);
        });
        using var cache = new MaterialThumbnailCache(transport);
        using var cancellation = new CancellationTokenSource();
        var first = cache.GetAsync(Key(), "material", cancellation.Token);
        var second = cache.GetAsync(Key(), "material");
        await Until(() => transport.Updates.Count == 1);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
        release.SetResult();
        Assert.Equal(new byte[] { 1, 2, 3, 4 }, await second);
        Assert.Single(transport.Updates);
        Assert.Single(transport.Closed);
    }

    [Fact]
    public async Task QueuedFieldUnloads_NoGpuWork_AndLaterLoadRetries()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new Transport(async (request, token) =>
        {
            await release.Task.WaitAsync(token);
            return Ready(request);
        });
        using var cache = new MaterialThumbnailCache(transport);
        var first = cache.GetAsync(Key(), "material");
        await Until(() => transport.Updates.Count == 1);
        using var cancellation = new CancellationTokenSource();
        var queued = cache.GetAsync(Key(asset: "second"), "material", cancellation.Token);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => queued);
        release.SetResult();
        await first;
        Assert.Single(transport.Updates);
        Assert.NotNull(await cache.GetAsync(Key(asset: "second"), "material"));
        Assert.Equal(2, transport.Updates.Count);
        Assert.Equal(2, transport.Closed.Count);
    }

    [Fact]
    public async Task MultipleColdReferences_RenderSerially_AndUseDistinctSessions()
    {
        var active = 0;
        var maximumActive = 0;
        var transport = new Transport(async (request, token) =>
        {
            maximumActive = Math.Max(maximumActive, Interlocked.Increment(ref active));
            try
            {
                await Task.Delay(10, token);
                return Ready(request);
            }
            finally
            {
                Interlocked.Decrement(ref active);
            }
        });
        using var cache = new MaterialThumbnailCache(transport);
        await Task.WhenAll(Enumerable.Range(0, 12)
            .Select(i => cache.GetAsync(Key(asset: i.ToString()), "material")));
        Assert.Equal(1, maximumActive);
        Assert.Equal(12, transport.Updates.Select(update => update.SessionId).Distinct().Count());
        Assert.Equal(12, transport.Closed.Count);
    }

    [Fact]
    public async Task WorkspaceReset_CancelsOldWork_AndNewGenerationCanRetry()
    {
        var transport = new Transport(async (request, token) =>
        {
            if (request.Snapshot.MaterialYaml == "old")
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, token);
            }
            return Ready(request);
        });
        using var cache = new MaterialThumbnailCache(transport);
        var old = cache.GetAsync(Key(), "old");
        await Until(() => transport.Updates.Count == 1);
        cache.Clear();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => old);
        Assert.NotNull(await cache.GetAsync(Key(workspace: "second", generation: 2), "new"));
        Assert.Equal(2, transport.Closed.Count);
    }

    [Fact]
    public async Task ContentWorkspaceAndGeneration_AreIndependentCacheKeys()
    {
        var transport = new Transport();
        using var cache = new MaterialThumbnailCache(transport);
        var keys = new[]
        {
            Key(), Key(yaml: "unsaved"), Key(workspace: "other"), Key(generation: 2)
        };
        foreach (var key in keys)
        {
            await cache.GetAsync(key, key.ContentHash);
        }
        foreach (var key in keys)
        {
            await cache.GetAsync(key, key.ContentHash);
        }
        Assert.Equal(4, transport.Updates.Count);
    }

    [Fact]
    public async Task DraftChangesDuringColdRender_CanceledImageCannotPopulateNewDraft()
    {
        var oldResponse = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new Transport(async (request, _) =>
        {
            if (request.Snapshot.MaterialYaml == "old")
            {
                // Native work may finish after its field has canceled the response.
                await oldResponse.Task;
            }
            return Ready(request) with { Png = request.Snapshot.MaterialYaml == "old" ? [1] : [9] };
        });
        using var cache = new MaterialThumbnailCache(transport);
        using var cancellation = new CancellationTokenSource();
        var old = cache.GetAsync(Key(yaml: "old"), "old", cancellation.Token);
        await Until(() => transport.Updates.Count == 1);
        cancellation.Cancel();
        var current = cache.GetAsync(Key(yaml: "new"), "new");
        oldResponse.SetResult();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => old);
        Assert.Equal(new byte[] { 9 }, await current);
        Assert.Equal(new byte[] { 9 }, await cache.GetAsync(Key(yaml: "new"), "new"));
        Assert.Equal(2, transport.Updates.Count);
        Assert.Equal(2, transport.Closed.Count);
    }

    [Fact]
    public async Task FailedRender_IsNotCached_AndAlwaysClosesItsSession()
    {
        var attempts = 0;
        var transport = new Transport((request, _) => Task.FromResult(
            Interlocked.Increment(ref attempts) == 1
                ? new MaterialPreviewResponse(request.SessionId, request.Revision, PreviewState.Error, [], "Shader unavailable")
                : Ready(request)));
        using var cache = new MaterialThumbnailCache(transport);
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() => cache.GetAsync(Key(), "material"));
        Assert.Contains("Shader unavailable", exception.Message);
        Assert.NotNull(await cache.GetAsync(Key(), "material"));
        Assert.Equal(2, transport.Updates.Count);
        Assert.Equal(2, transport.Closed.Count);
    }

    [Fact]
    public async Task MemoryLimit_EvictsLeastRecentlyUsedPixels()
    {
        var transport = new Transport();
        using var cache = new MaterialThumbnailCache(transport, maximumEntries: 4, maximumBytes: 8);
        await cache.GetAsync(Key(asset: "one"), "material");
        await cache.GetAsync(Key(asset: "two"), "material");
        await cache.GetAsync(Key(asset: "one"), "material");
        await cache.GetAsync(Key(asset: "three"), "material");
        await cache.GetAsync(Key(asset: "one"), "material");
        Assert.Equal(3, transport.Updates.Count);
        await cache.GetAsync(Key(asset: "two"), "material");
        Assert.Equal(4, transport.Updates.Count);
    }

    [Fact]
    public async Task ColdQueueAtCapacity_WaitsAndEventuallyRendersEveryField()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new Transport(async (request, token) =>
        {
            await release.Task.WaitAsync(token);
            return Ready(request);
        });
        using var cache = new MaterialThumbnailCache(transport, maximumEntries: 2);
        var first = cache.GetAsync(Key(asset: "one"), "material");
        var second = cache.GetAsync(Key(asset: "two"), "material");
        var waiting = Enumerable.Range(3, 12)
            .Select(i => cache.GetAsync(Key(asset: i.ToString()), "material")).ToArray();
        Assert.All(waiting, request => Assert.False(request.IsCompleted));
        release.SetResult();
        var images = await Task.WhenAll(waiting.Append(first).Append(second)).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.All(images, image => Assert.NotNull(image));
        Assert.Equal(14, transport.Updates.Count);
        Assert.Equal(14, transport.Closed.Count);
    }

    [Fact]
    public async Task FieldUnloadsWhileWaitingForAdmission_NoNativeRequestIsIssued()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new Transport(async (request, token) =>
        {
            await release.Task.WaitAsync(token);
            return Ready(request);
        });
        using var cache = new MaterialThumbnailCache(transport, maximumEntries: 1);
        var first = cache.GetAsync(Key(asset: "one"), "material");
        using var cancellation = new CancellationTokenSource();
        var waiting = cache.GetAsync(Key(asset: "two"), "material", cancellation.Token);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting);
        release.SetResult();
        Assert.NotNull(await first);
        Assert.Single(transport.Updates);
    }

    [Fact]
    public async Task ResetWhileWaitingForAdmission_CancelsOldWorkspaceWithoutStartingItsQueuedField()
    {
        var transport = new Transport(async (request, token) =>
        {
            if (request.Snapshot.MaterialYaml == "old")
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, token);
            }
            return Ready(request);
        });
        using var cache = new MaterialThumbnailCache(transport, maximumEntries: 1);
        var first = cache.GetAsync(Key(asset: "one"), "old");
        await Until(() => transport.Updates.Count == 1);
        var waiting = cache.GetAsync(Key(asset: "two"), "old");
        cache.Clear();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting);
        Assert.NotNull(await cache.GetAsync(Key(workspace: "new"), "new"));
        Assert.Equal(2, transport.Updates.Count);
    }

    static MaterialThumbnailKey Key(string workspace = "workspace", long generation = 1,
        string asset = "material", string yaml = "material") =>
        MaterialThumbnailKey.Create(workspace, generation, asset, yaml);

    static MaterialPreviewResponse Ready(MaterialPreviewRequest request) =>
        new(request.SessionId, request.Revision, PreviewState.Ready, [1, 2, 3, 4], string.Empty);

    static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        while (!condition())
        {
            await Task.Delay(5, timeout.Token);
        }
    }

    sealed class Transport(Func<MaterialPreviewRequest, CancellationToken, Task<MaterialPreviewResponse>>? update = null)
        : IMaterialPreviewTransport
    {
        public ConcurrentQueue<MaterialPreviewRequest> Updates { get; } = new();
        public ConcurrentQueue<ulong> Closed { get; } = new();

        public Task<MaterialPreviewResponse> UpdateAsync(MaterialPreviewRequest request,
            long engineGeneration, CancellationToken cancellationToken)
        {
            Updates.Enqueue(request);
            return update?.Invoke(request, cancellationToken) ?? Task.FromResult(Ready(request));
        }

        public Task<MaterialPreviewResponse> GetAsync(ulong sessionId, ulong afterRevision,
            long engineGeneration, CancellationToken cancellationToken) => throw new NotSupportedException();

        public Task CloseAsync(ulong sessionId, long engineGeneration, CancellationToken cancellationToken)
        {
            Closed.Enqueue(sessionId);
            return Task.CompletedTask;
        }
    }
}
