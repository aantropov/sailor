using SailorEditor.Controls;
using SailorEditor.Scene;

namespace Editor.Tests;

public sealed class NativeViewportInputQueueTests
{
    [Fact]
    public async Task Reset_CancelsInFlightInputAndDetachesPreviousTail()
    {
        var firstDispatchStarted =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDispatch =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var forwarded = new List<uint>();
        var queue = new NativeViewportInputQueue(
            async (input, cancellationToken) =>
            {
                if (input.KeyCode == 1)
                {
                    firstDispatchStarted.SetResult();
                    await releaseFirstDispatch.Task;
                }

                cancellationToken.ThrowIfCancellationRequested();
                forwarded.Add(input.KeyCode);
                return NativeViewportInputDispatchResult.Forwarded;
            });

        queue.Enqueue(Key(1));
        queue.Enqueue(Key(2));
        await firstDispatchStarted.Task;
        var previousTail = queue.WhenIdle;
        var previousEpoch = queue.Epoch;

        Assert.Equal(previousEpoch + 1, queue.Reset());
        queue.Enqueue(Key(3));
        await queue.WhenIdle;
        releaseFirstDispatch.SetResult();

        await previousTail;
        Assert.Equal([3U], forwarded);
    }

    [Fact]
    public async Task Reset_KeepsPreviousCancellationAliveUntilItsTailCompletes()
    {
        var firstDispatchStarted =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDispatch =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var registeredAfterReset = false;
        var queue = new NativeViewportInputQueue(
            async (_, cancellationToken) =>
            {
                firstDispatchStarted.SetResult();
                await releaseFirstDispatch.Task;
                using var registration = cancellationToken.Register(() => { });
                registeredAfterReset = true;
                cancellationToken.ThrowIfCancellationRequested();
                return NativeViewportInputDispatchResult.Forwarded;
            });

        queue.Enqueue(Key(1));
        await firstDispatchStarted.Task;
        var previousTail = queue.WhenIdle;

        queue.Reset();
        releaseFirstDispatch.SetResult();
        await previousTail;

        Assert.True(registeredAfterReset);
    }

    [Fact]
    public async Task ConsecutiveQueuedPointerMoves_CoalesceWithoutCrossingInputBoundaries()
    {
        var firstDispatchStarted =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDispatch =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var dispatched = new List<NativeSceneViewportInputEvent>();
        var queue = new NativeViewportInputQueue(
            async (input, _) =>
            {
                if (input.KeyCode == 1)
                {
                    firstDispatchStarted.SetResult();
                    await releaseFirstDispatch.Task;
                }

                dispatched.Add(input);
                return NativeViewportInputDispatchResult.Forwarded;
            });

        queue.Enqueue(Key(1));
        await firstDispatchStarted.Task;
        queue.Enqueue(Move(10));
        queue.Enqueue(Move(20));
        queue.Enqueue(Key(2));
        queue.Enqueue(Move(30));
        queue.Enqueue(Move(40));
        releaseFirstDispatch.SetResult();
        await queue.WhenIdle;

        Assert.Collection(
            dispatched,
            input => Assert.Equal(1U, input.KeyCode),
            input => Assert.Equal(20, input.PointerX),
            input => Assert.Equal(2U, input.KeyCode),
            input => Assert.Equal(40, input.PointerX));
    }

    [Fact]
    public async Task PointerMovesWithDifferentGestureState_AreNotCoalesced()
    {
        var firstDispatchStarted =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirstDispatch =
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var dispatched = new List<NativeSceneViewportInputEvent>();
        var queue = new NativeViewportInputQueue(
            async (input, _) =>
            {
                if (input.KeyCode == 1)
                {
                    firstDispatchStarted.SetResult();
                    await releaseFirstDispatch.Task;
                }

                dispatched.Add(input);
                return NativeViewportInputDispatchResult.Forwarded;
            });

        queue.Enqueue(Key(1));
        await firstDispatchStarted.Task;
        queue.Enqueue(Move(
            10,
            NativeSceneViewportInputModifier.MouseRight));
        queue.Enqueue(Move(
            20,
            NativeSceneViewportInputModifier.MouseMiddle));
        releaseFirstDispatch.SetResult();
        await queue.WhenIdle;

        Assert.Equal(3, dispatched.Count);
        Assert.Equal(
            NativeSceneViewportInputModifier.MouseRight,
            dispatched[1].Modifiers);
        Assert.Equal(
            NativeSceneViewportInputModifier.MouseMiddle,
            dispatched[2].Modifiers);
    }

    [Fact]
    public async Task RejectedPrimaryPress_SuppressesPointerGestureThroughMatchingRelease()
    {
        var dispatched = new List<NativeSceneViewportInputEvent>();
        var queue = new NativeViewportInputQueue(
            (input, _) =>
            {
                dispatched.Add(input);
                var result =
                    input.Kind == NativeSceneViewportInputKind.PointerButton &&
                    input.Button == 0 &&
                    input.Pressed
                        ? NativeViewportInputDispatchResult.RejectPrimaryPointerGesture
                        : NativeViewportInputDispatchResult.Forwarded;
                return Task.FromResult(result);
            });

        queue.Enqueue(PrimaryButton(pressed: true));
        queue.Enqueue(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.PointerMove,
            PointerX: 10));
        queue.Enqueue(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.PointerWheel,
            WheelDeltaY: 2));
        queue.Enqueue(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.Capture,
            Captured: true));
        queue.Enqueue(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.PointerButton,
            Button: 1,
            Pressed: true));
        queue.Enqueue(Key(7));
        queue.Enqueue(PrimaryButton(pressed: false));
        queue.Enqueue(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.PointerMove,
            PointerX: 20));

        await queue.WhenIdle;

        Assert.Collection(
            dispatched,
            input => Assert.True(input.Pressed),
            input => Assert.Equal(7U, input.KeyCode),
            input => Assert.Equal(20, input.PointerX));
    }

    [Fact]
    public async Task Reset_ClearsRejectedPrimaryGestureSuppression()
    {
        var dispatched = new List<NativeSceneViewportInputEvent>();
        var rejectPrimaryPress = true;
        var queue = new NativeViewportInputQueue(
            (input, _) =>
            {
                dispatched.Add(input);
                var result =
                    rejectPrimaryPress &&
                    input.Kind == NativeSceneViewportInputKind.PointerButton &&
                    input.Button == 0 &&
                    input.Pressed
                        ? NativeViewportInputDispatchResult.RejectPrimaryPointerGesture
                        : NativeViewportInputDispatchResult.Forwarded;
                return Task.FromResult(result);
            });

        queue.Enqueue(PrimaryButton(pressed: true));
        await queue.WhenIdle;
        rejectPrimaryPress = false;

        queue.Reset();
        queue.Enqueue(new NativeSceneViewportInputEvent(
            NativeSceneViewportInputKind.PointerMove,
            PointerX: 12));
        await queue.WhenIdle;

        Assert.Equal(2, dispatched.Count);
        Assert.Equal(12, dispatched[1].PointerX);
    }

    static NativeSceneViewportInputEvent Key(uint keyCode) =>
        new(
            NativeSceneViewportInputKind.Key,
            KeyCode: keyCode);

    static NativeSceneViewportInputEvent PrimaryButton(bool pressed) =>
        new(
            NativeSceneViewportInputKind.PointerButton,
            Button: 0,
            Pressed: pressed);

    static NativeSceneViewportInputEvent Move(
        float pointerX,
        NativeSceneViewportInputModifier modifiers =
            NativeSceneViewportInputModifier.None) =>
        new(
            NativeSceneViewportInputKind.PointerMove,
            PointerX: pointerX,
            Modifiers: modifiers);
}
