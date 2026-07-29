#nullable enable
using SailorEditor.Controls;

namespace SailorEditor.Scene;

public enum NativeViewportInputDispatchResult
{
    Forwarded = 0,
    RejectPrimaryPointerGesture,
}

public sealed class NativeViewportInputQueue(
    Func<NativeSceneViewportInputEvent, CancellationToken, Task<NativeViewportInputDispatchResult>> dispatchAsync,
    Action<Exception>? reportError = null)
{
    readonly object _sync = new();
    readonly Func<NativeSceneViewportInputEvent, CancellationToken, Task<NativeViewportInputDispatchResult>> _dispatchAsync =
        dispatchAsync ?? throw new ArgumentNullException(nameof(dispatchAsync));
    readonly Action<Exception>? _reportError = reportError;

    CancellationTokenSource _epochCancellation = new();
    Task _tail = Task.CompletedTask;
    QueuedInput? _coalesciblePointerMove;
    long _epoch;
    bool _suppressPrimaryPointerGesture;

    public long Epoch
    {
        get
        {
            lock (_sync)
            {
                return _epoch;
            }
        }
    }

    public Task WhenIdle
    {
        get
        {
            lock (_sync)
            {
                return _tail;
            }
        }
    }

    public void Enqueue(NativeSceneViewportInputEvent input)
    {
        lock (_sync)
        {
            if (_coalesciblePointerMove is not null &&
                CanCoalescePointerMove(
                    _coalesciblePointerMove.Input,
                    input))
            {
                _coalesciblePointerMove.Input = input;
                return;
            }

            var epoch = _epoch;
            var cancellationToken = _epochCancellation.Token;
            var queuedInput = new QueuedInput(
                input,
                epoch,
                cancellationToken);
            _coalesciblePointerMove =
                input.Kind == NativeSceneViewportInputKind.PointerMove
                    ? queuedInput
                    : null;
            _tail = ProcessAsync(
                _tail,
                queuedInput);
        }
    }

    public long Reset()
    {
        CancellationTokenSource previousCancellation;
        Task previousTail;
        long epoch;
        lock (_sync)
        {
            previousCancellation = _epochCancellation;
            previousTail = _tail;
            _epochCancellation = new CancellationTokenSource();
            epoch = ++_epoch;
            _tail = Task.CompletedTask;
            _coalesciblePointerMove = null;
            _suppressPrimaryPointerGesture = false;
        }

        previousCancellation.Cancel();
        _ = DisposeCancellationWhenCompletedAsync(
            previousCancellation,
            previousTail);
        return epoch;
    }

    async Task ProcessAsync(
        Task predecessor,
        QueuedInput queuedInput)
    {
        var cancellationToken = queuedInput.CancellationToken;
        try
        {
            await predecessor;
            cancellationToken.ThrowIfCancellationRequested();
            if (!TryBeginDispatch(
                    queuedInput,
                    out var input))
            {
                return;
            }

            var result = await _dispatchAsync(input, cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
            if (result == NativeViewportInputDispatchResult.RejectPrimaryPointerGesture &&
                IsPrimaryPointerPress(input))
            {
                SuppressPrimaryPointerGesture(queuedInput.Epoch);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            if (IsPrimaryPointerPress(queuedInput.Input))
            {
                SuppressPrimaryPointerGesture(queuedInput.Epoch);
            }
            _reportError?.Invoke(exception);
        }
    }

    bool TryBeginDispatch(
        QueuedInput queuedInput,
        out NativeSceneViewportInputEvent input)
    {
        lock (_sync)
        {
            input = queuedInput.Input;
            if (ReferenceEquals(
                    _coalesciblePointerMove,
                    queuedInput))
            {
                _coalesciblePointerMove = null;
            }

            if (queuedInput.Epoch != _epoch)
            {
                return false;
            }

            if (!_suppressPrimaryPointerGesture)
            {
                return true;
            }

            if (IsPrimaryPointerRelease(input))
            {
                _suppressPrimaryPointerGesture = false;
            }

            return !IsPointerGestureInput(input);
        }
    }

    void SuppressPrimaryPointerGesture(long epoch)
    {
        lock (_sync)
        {
            if (epoch == _epoch)
            {
                _suppressPrimaryPointerGesture = true;
            }
        }
    }

    static async Task DisposeCancellationWhenCompletedAsync(
        CancellationTokenSource cancellation,
        Task tail)
    {
        try
        {
            await tail.ConfigureAwait(false);
        }
        catch
        {
        }
        finally
        {
            cancellation.Dispose();
        }
    }

    static bool CanCoalescePointerMove(
        NativeSceneViewportInputEvent queued,
        NativeSceneViewportInputEvent incoming) =>
        queued.Kind == NativeSceneViewportInputKind.PointerMove &&
        incoming.Kind == NativeSceneViewportInputKind.PointerMove &&
        queued.Modifiers == incoming.Modifiers &&
        queued.Button == incoming.Button &&
        queued.KeyCode == incoming.KeyCode &&
        queued.Pressed == incoming.Pressed &&
        queued.Focused == incoming.Focused &&
        queued.Captured == incoming.Captured &&
        queued.WheelDeltaX == incoming.WheelDeltaX &&
        queued.WheelDeltaY == incoming.WheelDeltaY;

    static bool IsPointerGestureInput(
        NativeSceneViewportInputEvent input) =>
        input.Kind is NativeSceneViewportInputKind.PointerMove or
            NativeSceneViewportInputKind.PointerButton or
            NativeSceneViewportInputKind.PointerWheel or
            NativeSceneViewportInputKind.Capture;

    static bool IsPrimaryPointerPress(
        NativeSceneViewportInputEvent input) =>
        input.Kind == NativeSceneViewportInputKind.PointerButton &&
        input.Button == 0 &&
        input.Pressed;

    static bool IsPrimaryPointerRelease(
        NativeSceneViewportInputEvent input) =>
        input.Kind == NativeSceneViewportInputKind.PointerButton &&
        input.Button == 0 &&
        !input.Pressed;

    sealed class QueuedInput(
        NativeSceneViewportInputEvent input,
        long epoch,
        CancellationToken cancellationToken)
    {
        public NativeSceneViewportInputEvent Input { get; set; } = input;
        public long Epoch { get; } = epoch;
        public CancellationToken CancellationToken { get; } =
            cancellationToken;
    }
}
