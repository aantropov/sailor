using System.Security.Cryptography;
using System.Text;

namespace SailorEditor.Services;

internal sealed record MaterialThumbnailKey(
    string Workspace,
    long EngineGeneration,
    string AssetId,
    string ContentHash)
{
    public static MaterialThumbnailKey Create(
        string workspace, long engineGeneration, string assetId, string materialYaml) =>
        new(workspace, engineGeneration, assetId,
            Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(materialYaml))));
}

// Reference fields share a bounded cache and issue only one cold GPU request at a time.
internal sealed class MaterialThumbnailCache(IMaterialPreviewTransport transport,
    int maximumEntries = 128, int maximumBytes = 8 * 1024 * 1024) : IDisposable
{
    sealed class Entry
    {
        public CancellationTokenSource Cancellation { get; } = new();
        public Task<byte[]?> Task { get; set; } = null!;
        public int Waiters { get; set; }
        public int Bytes { get; set; }
        public long LastUse { get; set; }
        public bool Completed { get; set; }
    }

    readonly object gate = new();
    readonly SemaphoreSlim renderQueue = new(1, 1);
    readonly Dictionary<MaterialThumbnailKey, Entry> entries = [];
    TaskCompletionSource capacityChanged = new(TaskCreationOptions.RunContinuationsAsynchronously);
    long generation;
    long nextUse;
    int cachedBytes;
    bool disposed;

    public async Task<byte[]?> GetAsync(MaterialThumbnailKey key, string materialYaml,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Entry entry;
        long requestGeneration;
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            requestGeneration = generation;
        }
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Task? capacity = null;
            lock (gate)
            {
                ObjectDisposedException.ThrowIf(disposed, this);
                if (requestGeneration != generation)
                {
                    throw new OperationCanceledException("Thumbnail cache was invalidated.", cancellationToken);
                }
                if (!entries.TryGetValue(key, out entry!))
                {
                    TrimLocked(maximumEntries - 1, maximumBytes);
                    if (entries.Count >= maximumEntries)
                    {
                        // Waiting fields hold no cache slot or native session and can unload at any time.
                        capacity = capacityChanged.Task;
                    }
                    else
                    {
                        entry = new Entry();
                        entries.Add(key, entry);
                        entry.Task = Task.Run(() => RenderAsync(key, materialYaml, entry));
                    }
                }
                if (capacity is null)
                {
                    entry.Waiters++;
                    entry.LastUse = ++nextUse;
                    break;
                }
            }
            await capacity.WaitAsync(cancellationToken).ConfigureAwait(false);
        }

        try
        {
            return await entry.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            lock (gate)
            {
                entry.Waiters--;
                if (entry.Waiters == 0 && !entry.Completed)
                {
                    entry.Cancellation.Cancel();
                    RemoveLocked(key, entry);
                }
            }
        }
    }

    async Task<byte[]?> RenderAsync(MaterialThumbnailKey key, string yaml, Entry entry)
    {
        var token = entry.Cancellation.Token;
        try
        {
            await renderQueue.WaitAsync(token).ConfigureAwait(false);
            byte[]? png;
            try
            {
                token.ThrowIfCancellationRequested();
                await using var controller = new MaterialPreviewController(transport,
                    debounce: TimeSpan.Zero, pollInterval: TimeSpan.FromMilliseconds(20));
                var ready = new TaskCompletionSource<byte[]?>(TaskCreationOptions.RunContinuationsAsynchronously);
                controller.Changed += () =>
                {
                    var presentation = controller.Presentation;
                    if (presentation.RequestedRevision > 0 && !presentation.IsLoading)
                    {
                        if (presentation.Png is { Length: > 0 } image)
                        {
                            ready.TrySetResult(image);
                        }
                        else
                        {
                            ready.TrySetException(new InvalidOperationException(presentation.Diagnostic));
                        }
                    }
                };
                controller.SetContext(entry, key.EngineGeneration, true);
                controller.Update(new MaterialPreviewSnapshot(yaml, Width: 128, Height: 128));
                png = await ready.Task.WaitAsync(token).ConfigureAwait(false);
            }
            finally
            {
                renderQueue.Release();
            }

            lock (gate)
            {
                token.ThrowIfCancellationRequested();
                if (entries.TryGetValue(key, out var current) && ReferenceEquals(current, entry))
                {
                    entry.Bytes = png?.Length ?? 0;
                    cachedBytes += entry.Bytes;
                    entry.Completed = true;
                    if (entry.Bytes == 0 || entry.Bytes > maximumBytes)
                    {
                        RemoveLocked(key, entry);
                    }
                    TrimLocked(maximumEntries, maximumBytes);
                    NotifyCapacityLocked();
                }
            }
            return png;
        }
        catch
        {
            lock (gate)
            {
                RemoveLocked(key, entry);
            }
            throw;
        }
        finally
        {
            lock (gate)
            {
                entry.Completed = true;
                entry.Cancellation.Dispose();
            }
        }
    }

    void RemoveLocked(MaterialThumbnailKey key, Entry entry)
    {
        if (entries.TryGetValue(key, out var current) && ReferenceEquals(current, entry))
        {
            entries.Remove(key);
            cachedBytes -= entry.Bytes;
            NotifyCapacityLocked();
        }
    }

    void NotifyCapacityLocked()
    {
        var previous = capacityChanged;
        capacityChanged = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        previous.TrySetResult();
    }

    void TrimLocked(int countLimit, int byteLimit)
    {
        foreach (var candidate in entries.Where(pair => pair.Value.Completed)
                     .OrderBy(pair => pair.Value.LastUse).ToArray())
        {
            if (entries.Count <= countLimit && cachedBytes <= byteLimit)
            {
                break;
            }
            RemoveLocked(candidate.Key, candidate.Value);
        }
    }

    public void Clear()
    {
        lock (gate)
        {
            generation++;
            foreach (var entry in entries.Values)
            {
                if (!entry.Completed)
                {
                    entry.Cancellation.Cancel();
                }
            }
            entries.Clear();
            cachedBytes = 0;
            NotifyCapacityLocked();
        }
    }

    public void Dispose()
    {
        lock (gate)
        {
            disposed = true;
            Clear();
        }
    }
}
