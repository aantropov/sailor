namespace SailorEditor.Services;

internal enum PreviewPrimitive
{
    Sphere = 1,
    Plane = 2,
    Box = 3
}

internal enum PreviewState
{
    Pending = 1,
    Ready = 2,
    Error = 3,
    Closed = 4
}

internal sealed record MaterialPreviewSnapshot(
    string MaterialYaml,
    PreviewPrimitive Primitive = PreviewPrimitive.Sphere,
    float Yaw = 0.45f,
    float Pitch = 0.25f,
    float Distance = 3.0f,
    uint Width = 384,
    uint Height = 384,
    string ModelFileId = "");

internal sealed record MaterialPreviewRequest(
    ulong SessionId,
    ulong Revision,
    MaterialPreviewSnapshot Snapshot);

internal sealed record MaterialPreviewResponse(
    ulong SessionId,
    ulong Revision,
    PreviewState State,
    byte[] Png,
    string Diagnostic);

internal interface IMaterialPreviewTransport
{
    Task<MaterialPreviewResponse> UpdateAsync(
        MaterialPreviewRequest request, long engineGeneration, CancellationToken cancellationToken);
    Task<MaterialPreviewResponse> GetAsync(
        ulong sessionId, ulong afterRevision, long engineGeneration, CancellationToken cancellationToken);
    Task CloseAsync(ulong sessionId, long engineGeneration, CancellationToken cancellationToken);
}

internal sealed record MaterialPreviewPresentation(
    ulong SessionId,
    ulong RequestedRevision,
    ulong ImageRevision,
    byte[]? Png,
    bool IsLoading,
    string Diagnostic);

// Owns one serialized request pipeline. The view supplies immutable snapshots on the UI thread.
internal sealed class MaterialPreviewController : IAsyncDisposable
{
    sealed record Session(object Binding, long EngineGeneration, ulong Id)
    {
        public CancellationTokenSource Cancellation { get; } = new();
        public ulong NextRevision { get; set; }
    }

    sealed record Work(Session Session, MaterialPreviewRequest Request);

    static long nextSessionId = DateTime.UtcNow.Ticks;
    readonly object gate = new();
    readonly IMaterialPreviewTransport transport;
    readonly TimeSpan debounce;
    readonly TimeSpan pollInterval;
    readonly TimeSpan requestTimeout;
    readonly Queue<Session> closing = new();
    Session? session;
    Work? desired;
    Work? handled;
    CancellationTokenSource? activeOperation;
    Task processing = Task.CompletedTask;
    bool processingActive;
    bool disposed;
    MaterialPreviewPresentation presentation = new(0, 0, 0, null, false, string.Empty);

    public MaterialPreviewController(
        IMaterialPreviewTransport transport,
        TimeSpan? debounce = null,
        TimeSpan? pollInterval = null,
        TimeSpan? requestTimeout = null)
    {
        this.transport = transport;
        this.debounce = debounce ?? TimeSpan.FromMilliseconds(100);
        this.pollInterval = pollInterval ?? TimeSpan.FromMilliseconds(16);
        this.requestTimeout = requestTimeout ?? TimeSpan.FromSeconds(30);
    }

    public event Action? Changed;

    public MaterialPreviewPresentation Presentation
    {
        get
        {
            lock (gate)
            {
                return presentation;
            }
        }
    }

    public bool SetContext(object? binding, long engineGeneration, bool active)
    {
        lock (gate)
        {
            ObjectDisposedException.ThrowIf(disposed, this);
            if (active && binding is not null && session is not null &&
                ReferenceEquals(session.Binding, binding) && session.EngineGeneration == engineGeneration)
            {
                return false;
            }
            if ((!active || binding is null) && session is null)
            {
                return false;
            }

            CloseSessionLocked();
            if (active && binding is not null)
            {
                session = new Session(binding, engineGeneration, checked((ulong)Interlocked.Increment(ref nextSessionId)));
            }
            presentation = new(session?.Id ?? 0, 0, 0, null, false, string.Empty);
            StartProcessingLocked();
        }
        Changed?.Invoke();
        return true;
    }

    public void Update(MaterialPreviewSnapshot snapshot)
    {
        lock (gate)
        {
            if (disposed || session is null || desired?.Request.Snapshot == snapshot)
            {
                return;
            }
            if (desired is not null && !HasSameContent(desired.Request.Snapshot, snapshot))
            {
                activeOperation?.Cancel();
            }
            desired = new Work(session, new MaterialPreviewRequest(session.Id, ++session.NextRevision, snapshot));
            presentation = presentation with
            {
                RequestedRevision = desired.Request.Revision,
                IsLoading = true,
                Diagnostic = string.Empty
            };
            StartProcessingLocked();
        }
        Changed?.Invoke();
    }

    public void ReportSnapshotError(string diagnostic)
    {
        lock (gate)
        {
            if (disposed || session is null)
            {
                return;
            }
            activeOperation?.Cancel();
            desired = null;
            presentation = presentation with { IsLoading = false, Diagnostic = diagnostic };
        }
        Changed?.Invoke();
    }

    void CloseSessionLocked()
    {
        activeOperation?.Cancel();
        if (session is not null)
        {
            session.Cancellation.Cancel();
            closing.Enqueue(session);
        }
        session = null;
        desired = null;
        handled = null;
    }

    void StartProcessingLocked()
    {
        if (processingActive || (closing.Count == 0 && (desired is null || ReferenceEquals(desired, handled))))
        {
            return;
        }
        processingActive = true;
        processing = Task.Run(ProcessAsync);
    }

    async Task ProcessAsync()
    {
        while (true)
        {
            Session? toClose;
            Work? work;
            Work? previous;
            lock (gate)
            {
                toClose = closing.Count > 0 ? closing.Dequeue() : null;
                work = desired;
                previous = handled;
                if (toClose is null && (work is null || ReferenceEquals(work, handled)))
                {
                    processingActive = false;
                    return;
                }
            }

            if (toClose is not null)
            {
                try
                {
                    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
                    await transport.CloseAsync(toClose.Id, toClose.EngineGeneration, timeout.Token).ConfigureAwait(false);
                }
                catch (Exception exception)
                {
                    Console.Error.WriteLine($"Material preview cleanup: {exception.Message}");
                }
                finally
                {
                    toClose.Cancellation.Dispose();
                }
                continue;
            }

            CancellationTokenSource? operation = null;
            try
            {
                var delay = previous is not null && HasSameContent(previous.Request.Snapshot, work!.Request.Snapshot)
                    ? TimeSpan.Zero
                    : debounce;
                await Task.Delay(delay, work!.Session.Cancellation.Token).ConfigureAwait(false);
                ulong afterRevision;
                lock (gate)
                {
                    if (desired is null || !ReferenceEquals(session, work.Session))
                    {
                        continue;
                    }
                    work = desired;
                    handled = work;
                    operation = CancellationTokenSource.CreateLinkedTokenSource(work.Session.Cancellation.Token);
                    operation.CancelAfter(requestTimeout);
                    activeOperation = operation;
                    afterRevision = presentation.ImageRevision;
                }

                var response = await transport.UpdateAsync(
                    work.Request, work.Session.EngineGeneration, operation.Token).ConfigureAwait(false);
                while (IsCurrent(work) && response.State == PreviewState.Pending)
                {
                    if (response.SessionId != work.Session.Id)
                    {
                        throw new InvalidOperationException("Material preview returned a different session.");
                    }
                    await Task.Delay(pollInterval, operation.Token).ConfigureAwait(false);
                    response = await transport.GetAsync(
                        work.Session.Id, afterRevision, work.Session.EngineGeneration, operation.Token).ConfigureAwait(false);
                }

                if (!IsCurrent(work))
                {
                    continue;
                }
                if (response.SessionId != work.Session.Id || response.Revision != work.Request.Revision)
                {
                    throw new InvalidOperationException("Material preview returned a stale revision.");
                }
                if (response.State == PreviewState.Ready && response.Png.Length > 0)
                {
                    Publish(work, response.Png, string.Empty);
                }
                else
                {
                    Publish(work, null, string.IsNullOrWhiteSpace(response.Diagnostic)
                        ? "Material preview could not be rendered."
                        : response.Diagnostic);
                }
            }
            catch (OperationCanceledException)
            {
                if (work is not null && IsCurrent(work))
                {
                    Publish(work, null, "Material preview timed out. Change a setting to retry.");
                }
            }
            catch (Exception exception)
            {
                if (work is not null)
                {
                    Publish(work, null, exception.Message);
                }
            }
            finally
            {
                lock (gate)
                {
                    if (ReferenceEquals(activeOperation, operation))
                    {
                        activeOperation = null;
                    }
                    operation?.Dispose();
                }
            }
        }
    }

    bool IsCurrent(Work work)
    {
        lock (gate)
        {
            return !disposed && ReferenceEquals(session, work.Session) && desired is not null &&
                HasSameContent(desired.Request.Snapshot, work.Request.Snapshot);
        }
    }

    static bool HasSameContent(MaterialPreviewSnapshot left, MaterialPreviewSnapshot right) =>
        left.MaterialYaml == right.MaterialYaml && left.ModelFileId == right.ModelFileId &&
        left.Primitive == right.Primitive &&
        left.Width == right.Width && left.Height == right.Height;

    void Publish(Work work, byte[]? png, string diagnostic)
    {
        lock (gate)
        {
            if (!IsCurrent(work))
            {
                return;
            }
            presentation = presentation with
            {
                Png = png ?? presentation.Png,
                ImageRevision = png is null ? presentation.ImageRevision : work.Request.Revision,
                IsLoading = !ReferenceEquals(desired, work),
                Diagnostic = diagnostic
            };
        }
        Changed?.Invoke();
    }

    public async ValueTask DisposeAsync()
    {
        Task completion;
        lock (gate)
        {
            if (!disposed)
            {
                disposed = true;
                CloseSessionLocked();
                StartProcessingLocked();
            }
            completion = processing;
        }
        await completion.ConfigureAwait(false);
    }
}
