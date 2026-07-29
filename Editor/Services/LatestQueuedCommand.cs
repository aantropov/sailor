namespace SailorEditor.Services;

internal sealed class LatestQueuedCommand<T>
{
    readonly object gate = new();
    T latest = default!;
    bool hasValue;
    bool scheduled;
    long epoch;

    public bool Enqueue(
        T value,
        Func<Func<ValueTask<bool>>, bool> schedule,
        Func<T, ValueTask<bool>> execute)
    {
        ArgumentNullException.ThrowIfNull(schedule);
        ArgumentNullException.ThrowIfNull(execute);

        lock (gate)
        {
            latest = value;
            hasValue = true;
            if (scheduled)
            {
                return true;
            }

            var scheduledEpoch = epoch;
            scheduled = schedule(
                () => DrainAsync(scheduledEpoch, execute));
            if (!scheduled)
            {
                hasValue = false;
            }
            return scheduled;
        }
    }

    async ValueTask<bool> DrainAsync(
        long scheduledEpoch,
        Func<T, ValueTask<bool>> execute)
    {
        T value;
        lock (gate)
        {
            if (scheduledEpoch != epoch || !hasValue)
            {
                if (scheduledEpoch == epoch)
                {
                    scheduled = false;
                }
                return false;
            }

            value = latest;
            hasValue = false;
            // Mark this slot free before execution. If a newer value arrives
            // while execute is running, it is appended to the shared platform
            // queue immediately and therefore keeps its order relative to
            // later edge commands.
            scheduled = false;
        }

        return await execute(value).ConfigureAwait(false);
    }

    public void Reset()
    {
        lock (gate)
        {
            epoch++;
            hasValue = false;
            scheduled = false;
            latest = default!;
        }
    }
}

internal sealed class KeyedLatestQueuedCommand<TKey, TValue>
    where TKey : notnull
{
    readonly object gate = new();
    readonly Dictionary<TKey, LatestQueuedCommand<TValue>> commands = [];

    public bool Enqueue(
        TKey key,
        TValue value,
        Func<Func<ValueTask<bool>>, bool> schedule,
        Func<TValue, ValueTask<bool>> execute)
    {
        lock (gate)
        {
            if (!commands.TryGetValue(key, out var command))
            {
                command = new LatestQueuedCommand<TValue>();
                commands.Add(key, command);
            }

            return command.Enqueue(value, schedule, execute);
        }
    }

    public void Reset(TKey key)
    {
        lock (gate)
        {
            if (!commands.Remove(key, out var command))
            {
                return;
            }
            command.Reset();
        }
    }

    public void Reset()
    {
        lock (gate)
        {
            foreach (var command in commands.Values)
            {
                command.Reset();
            }
            commands.Clear();
        }
    }
}
