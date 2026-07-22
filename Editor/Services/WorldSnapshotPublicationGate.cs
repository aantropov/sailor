namespace SailorEditor.Services;

internal sealed class WorldSnapshotPublicationGate
{
    long _nextSequence;
    long _lastPublishedSequence;

    public long ReserveSequence() => Interlocked.Increment(ref _nextSequence);

    public bool TryAdvance(long sequence)
    {
        if (sequence <= 0)
            return false;

        while (true)
        {
            var lastPublished = Volatile.Read(ref _lastPublishedSequence);
            if (sequence <= lastPublished)
                return false;

            if (Interlocked.CompareExchange(ref _lastPublishedSequence, sequence, lastPublished) == lastPublished)
                return true;
        }
    }
}
