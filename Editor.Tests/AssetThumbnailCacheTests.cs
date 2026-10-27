using System.Collections.Concurrent;
using SailorEditor.Services;

namespace Editor.Tests;

public sealed class AssetThumbnailCacheTests
{
    [Fact]
    public async Task MaterialAndModelConsumers_ShareSerialQueue_AndDeduplicateOnlyMatchingKindAndExtent()
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
        using var cache = new AssetThumbnailCache(transport);
        var material = Key();
        var model = material with { Kind = AssetThumbnailKind.Model };
        var largeModel = model with { Width = 256, Height = 256 };
        var modelSnapshot = new MaterialPreviewSnapshot(string.Empty, Width: 128, Height: 128, ModelFileId: model.AssetId);
        await Task.WhenAll(
            Request(cache, material, "material"),
            cache.GetAsync(model, modelSnapshot),
            cache.GetAsync(model, modelSnapshot),
            cache.GetAsync(largeModel, modelSnapshot with { Width = 256, Height = 256 }));
        Assert.Equal(1, maximumActive);
        Assert.Equal(3, transport.Updates.Count);
        Assert.Single(transport.Updates, update => update.Snapshot.ModelFileId.Length == 0);
        Assert.Single(transport.Updates, update => update.Snapshot.ModelFileId.Length > 0 && update.Snapshot.Width == 128);
        Assert.Single(transport.Updates, update => update.Snapshot.ModelFileId.Length > 0 && update.Snapshot.Width == 256);
        Assert.Equal(3, transport.Closed.Count);
    }

    [Fact]
    public async Task DependencyInvalidation_RejectsLateModelImage_EvenWhenModelSourceKeyIsUnchanged()
    {
        var oldImage = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var requests = 0;
        var transport = new Transport(async (request, _) =>
        {
            var revision = Interlocked.Increment(ref requests);
            if (revision == 1)
            {
                await oldImage.Task;
            }
            return Ready(request) with { Png = [(byte)revision] };
        });
        using var cache = new AssetThumbnailCache(transport);
        var key = Key() with { Kind = AssetThumbnailKind.Model, ContentGeneration = 1 };
        var snapshot = new MaterialPreviewSnapshot(string.Empty, Width: 128, Height: 128, ModelFileId: key.AssetId);
        var old = cache.GetAsync(key, snapshot);
        await Until(() => transport.Updates.Count == 1);
        cache.Clear();
        var currentKey = key with { ContentGeneration = 2 };
        var current = cache.GetAsync(currentKey, snapshot);
        oldImage.SetResult();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => old);
        Assert.Equal(new byte[] { 2 }, await current);
        Assert.Equal(new byte[] { 2 }, await cache.GetAsync(currentKey, snapshot));
        Assert.Equal(2, transport.Updates.Count);
    }

    [Fact]
    public async Task ModelRevisionKey_ChangesWhenSavedSourceOrMetadataChanges()
    {
        var directory = Directory.CreateTempSubdirectory("sailor-model-preview-key-");
        try
        {
            var source = Path.Combine(directory.FullName, "model.gltf");
            var metadata = source + ".asset";
            await File.WriteAllTextAsync(source, "geometry");
            await File.WriteAllTextAsync(metadata, "material A");
            Task<AssetThumbnailKey> Read() => AssetThumbnailKey.CreateModelAsync("workspace", 4, "model",
                source, metadata, 256, CancellationToken.None);
            var initial = await Read();
            Assert.Equal(initial, await Read());
            await File.WriteAllTextAsync(source, "replacement geometry");
            var geometryChanged = await Read();
            Assert.NotEqual(initial, geometryChanged);
            await File.WriteAllTextAsync(metadata, "material B plus import settings");
            Assert.NotEqual(geometryChanged, await Read());
            Assert.Equal(AssetThumbnailKind.Model, initial.Kind);
            Assert.Equal(256u, initial.Width);
        }
        finally
        {
            directory.Delete(true);
        }
    }

    [Fact]
    public async Task SameDraft_DeduplicatesRequests_CachesPixels_AndClosesSession()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var transport = new Transport(async (request, token) =>
        {
            await release.Task.WaitAsync(token);
            return Ready(request);
        });
        using var cache = new AssetThumbnailCache(transport);
        var key = Key();
        var first = Request(cache, key, "material");
        var second = Request(cache, key, "material");
        await Until(() => transport.Updates.Count == 1);
        release.SetResult();
        Assert.Equal(await first, await second);
        Assert.Equal(await first, await Request(cache, key, "material"));
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
        using var cache = new AssetThumbnailCache(transport);
        using var cancellation = new CancellationTokenSource();
        var first = Request(cache, Key(), "material", cancellation.Token);
        var second = Request(cache, Key(), "material");
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
        using var cache = new AssetThumbnailCache(transport);
        var first = Request(cache, Key(), "material");
        await Until(() => transport.Updates.Count == 1);
        using var cancellation = new CancellationTokenSource();
        var queued = Request(cache, Key(asset: "second"), "material", cancellation.Token);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => queued);
        release.SetResult();
        await first;
        Assert.Single(transport.Updates);
        Assert.NotNull(await Request(cache, Key(asset: "second"), "material"));
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
        using var cache = new AssetThumbnailCache(transport);
        await Task.WhenAll(Enumerable.Range(0, 12)
            .Select(i => Request(cache, Key(asset: i.ToString()), "material")));
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
        using var cache = new AssetThumbnailCache(transport);
        var old = Request(cache, Key(), "old");
        await Until(() => transport.Updates.Count == 1);
        cache.Clear();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => old);
        Assert.NotNull(await Request(cache, Key(workspace: "second", generation: 2), "new"));
        Assert.Equal(2, transport.Closed.Count);
    }

    [Fact]
    public async Task ContentWorkspaceAndGeneration_AreIndependentCacheKeys()
    {
        var transport = new Transport();
        using var cache = new AssetThumbnailCache(transport);
        var keys = new[]
        {
            Key(), Key(yaml: "unsaved"), Key(workspace: "other"), Key(generation: 2)
        };
        foreach (var key in keys)
        {
            await Request(cache, key, key.ContentHash);
        }
        foreach (var key in keys)
        {
            await Request(cache, key, key.ContentHash);
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
        using var cache = new AssetThumbnailCache(transport);
        using var cancellation = new CancellationTokenSource();
        var old = Request(cache, Key(yaml: "old"), "old", cancellation.Token);
        await Until(() => transport.Updates.Count == 1);
        cancellation.Cancel();
        var current = Request(cache, Key(yaml: "new"), "new");
        oldResponse.SetResult();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => old);
        Assert.Equal(new byte[] { 9 }, await current);
        Assert.Equal(new byte[] { 9 }, await Request(cache, Key(yaml: "new"), "new"));
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
        using var cache = new AssetThumbnailCache(transport);
        var exception = await Assert.ThrowsAsync<InvalidOperationException>(() => Request(cache, Key(), "material"));
        Assert.Contains("Shader unavailable", exception.Message);
        Assert.NotNull(await Request(cache, Key(), "material"));
        Assert.Equal(2, transport.Updates.Count);
        Assert.Equal(2, transport.Closed.Count);
    }

    [Fact]
    public async Task MemoryLimit_EvictsLeastRecentlyUsedPixels()
    {
        var transport = new Transport();
        using var cache = new AssetThumbnailCache(transport, maximumEntries: 4, maximumBytes: 8);
        await Request(cache, Key(asset: "one"), "material");
        await Request(cache, Key(asset: "two"), "material");
        await Request(cache, Key(asset: "one"), "material");
        await Request(cache, Key(asset: "three"), "material");
        await Request(cache, Key(asset: "one"), "material");
        Assert.Equal(3, transport.Updates.Count);
        await Request(cache, Key(asset: "two"), "material");
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
        using var cache = new AssetThumbnailCache(transport, maximumEntries: 2);
        var first = Request(cache, Key(asset: "one"), "material");
        var second = Request(cache, Key(asset: "two"), "material");
        var waiting = Enumerable.Range(3, 12)
            .Select(i => Request(cache, Key(asset: i.ToString()), "material")).ToArray();
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
        using var cache = new AssetThumbnailCache(transport, maximumEntries: 1);
        var first = Request(cache, Key(asset: "one"), "material");
        using var cancellation = new CancellationTokenSource();
        var waiting = Request(cache, Key(asset: "two"), "material", cancellation.Token);
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
        using var cache = new AssetThumbnailCache(transport, maximumEntries: 1);
        var first = Request(cache, Key(asset: "one"), "old");
        await Until(() => transport.Updates.Count == 1);
        var waiting = Request(cache, Key(asset: "two"), "old");
        cache.Clear();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting);
        Assert.NotNull(await Request(cache, Key(workspace: "new"), "new"));
        Assert.Equal(2, transport.Updates.Count);
    }

    static AssetThumbnailKey Key(string workspace = "workspace", long generation = 1,
        string asset = "material", string yaml = "material") =>
        AssetThumbnailKey.Create(AssetThumbnailKind.Material, workspace, generation, asset, yaml);

    static Task<byte[]?> Request(AssetThumbnailCache cache, AssetThumbnailKey key, string yaml,
        CancellationToken cancellationToken = default) =>
        cache.GetAsync(key, new MaterialPreviewSnapshot(yaml, Width: key.Width, Height: key.Height), cancellationToken);

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
