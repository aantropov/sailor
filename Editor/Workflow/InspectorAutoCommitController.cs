namespace SailorEditor.Workflow;

public readonly record struct InspectorAutoCommitDecision(bool MarkDirty)
{
    public static InspectorAutoCommitDecision None { get; } = new(false);
}

public sealed class InspectorAutoCommitController
{
    readonly Func<string?, bool> _shouldIgnoreProperty;

    bool _isInitialized;

    public InspectorAutoCommitController(Func<string?, bool> shouldIgnoreProperty, Func<string?, bool> shouldCommitProperty)
    {
        _shouldIgnoreProperty = shouldIgnoreProperty;
    }

    public InspectorAutoCommitDecision OnPropertyChanged(string? propertyName)
    {
        if (!_isInitialized || _shouldIgnoreProperty(propertyName))
            return InspectorAutoCommitDecision.None;

        return new InspectorAutoCommitDecision(MarkDirty: true);
    }

    public bool ShouldCommitPendingChanges(bool isDirty) => _isInitialized && isDirty;

    public void MarkInitialized()
    {
        _isInitialized = true;
    }
}
