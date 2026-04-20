using System.Diagnostics;

namespace SailorEditor.Utility;

public readonly record struct EditorPerfSample(string Name, TimeSpan Duration)
{
    public double DurationMs => Duration.TotalMilliseconds;

    public override string ToString() => $"[EditorPerf] {Name}: {DurationMs:F2} ms";
}

public static class EditorPerf
{
    static readonly RingBufferedBatcher<EditorPerfSample> _samples = new(256);
    static Action<EditorPerfSample> _sink = sample => Debug.WriteLine(sample.ToString());

    public static Action<EditorPerfSample> Sink
    {
        get => _sink;
        set => _sink = value ?? (_ => { });
    }

    public static EditorPerfScope Scope(string name) => new(name, Record);

    public static EditorPerfSample[] Snapshot() => _samples.Snapshot();

    static void Record(EditorPerfSample sample)
    {
        _samples.EnqueueRange([sample]);
        Sink(sample);
    }
}

public sealed class EditorPerfScope : IDisposable
{
    readonly string _name;
    readonly Action<EditorPerfSample> _sink;
    readonly long _startedAt;
    bool _disposed;

    public EditorPerfScope(string name, Action<EditorPerfSample>? sink = null)
    {
        _name = string.IsNullOrWhiteSpace(name) ? "unnamed" : name;
        _sink = sink ?? EditorPerf.Sink;
        _startedAt = Stopwatch.GetTimestamp();
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        var elapsed = Stopwatch.GetElapsedTime(_startedAt);
        _sink(new EditorPerfSample(_name, elapsed));
    }
}

public sealed class RingBufferedBatcher<T>
{
    readonly object _lock = new();
    readonly List<T> _history = [];
    readonly List<T> _pending = [];
    readonly int _capacity;

    public RingBufferedBatcher(int capacity)
    {
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));

        _capacity = capacity;
    }

    public int Capacity => _capacity;

    public void EnqueueRange(IEnumerable<T> items)
    {
        ArgumentNullException.ThrowIfNull(items);

        lock (_lock)
        {
            foreach (var item in items)
            {
                _history.Add(item);
                _pending.Add(item);
            }

            var overflow = _history.Count - _capacity;
            if (overflow > 0)
                _history.RemoveRange(0, overflow);
        }
    }

    public T[] Snapshot()
    {
        lock (_lock)
        {
            return _history.ToArray();
        }
    }

    public bool HasPending
    {
        get
        {
            lock (_lock)
            {
                return _pending.Count > 0;
            }
        }
    }

    public T[] DrainPending()
    {
        lock (_lock)
        {
            if (_pending.Count == 0)
                return [];

            var result = _pending.ToArray();
            _pending.Clear();
            return result;
        }
    }
}
