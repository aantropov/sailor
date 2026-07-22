namespace SailorEditor.Workflow;

public readonly record struct InspectorAutoCommitDecision(bool MarkDirty, bool CommitNow)
{
    public static InspectorAutoCommitDecision None { get; } = new(false, false);
}

public sealed class InspectorAutoCommitController
{
    readonly Func<string?, bool> _shouldIgnoreProperty;
    readonly Func<string?, bool> _shouldCommitProperty;

    bool _isInitialized;

    public InspectorAutoCommitController(Func<string?, bool> shouldIgnoreProperty, Func<string?, bool> shouldCommitProperty)
    {
        _shouldIgnoreProperty = shouldIgnoreProperty;
        _shouldCommitProperty = shouldCommitProperty;
    }

    public InspectorAutoCommitDecision OnPropertyChanged(string? propertyName)
    {
        if (!_isInitialized || _shouldIgnoreProperty(propertyName))
            return InspectorAutoCommitDecision.None;

        return new InspectorAutoCommitDecision(
            MarkDirty: true,
            CommitNow: _shouldCommitProperty(propertyName));
    }

    public bool ShouldCommitPendingChanges(bool isDirty) => _isInitialized && isDirty;

    public void MarkInitialized()
    {
        _isInitialized = true;
    }
}
