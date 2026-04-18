namespace SailorEditor.Panels;

public enum PanelRole
{
    Tool,
    Document,
}

public readonly record struct PanelId(string Value)
{
    public static PanelId New() => new(Guid.NewGuid().ToString("N"));

    public bool IsEmpty => string.IsNullOrWhiteSpace(Value);

    public override string ToString() => Value ?? string.Empty;
}

public readonly record struct PanelTypeId(string Value)
{
    public bool IsEmpty => string.IsNullOrWhiteSpace(Value);

    public override string ToString() => Value ?? string.Empty;
}

public readonly record struct PanelDockPreference(string? TargetGroupId = null, DockPosition DefaultPosition = DockPosition.Center);

public enum DockPosition
{
    Left,
    Top,
    Right,
    Bottom,
    Center,
    Floating,
}

public sealed record PanelDescriptor(
    PanelTypeId TypeId,
    string Title,
    PanelRole Role,
    bool AllowMultipleInstances,
    PanelDockPreference DefaultDockPreference,
    Func<object>? Factory = null,
    IPanelPersistenceStrategy? PersistenceStrategy = null,
    string? Icon = null)
{
    public bool IsSingleton => !AllowMultipleInstances;
}

public sealed record PanelInstanceState(
    PanelId PanelId,
    PanelTypeId TypeId,
    string? TitleOverride = null,
    string? PersistenceKey = null,
    IReadOnlyDictionary<string, string?>? Properties = null);

public interface IPanelPersistenceStrategy
{
    string? CreatePersistenceKey(PanelInstanceState panel);

    PanelInstanceState Restore(PanelTypeId panelTypeId, string persistenceKey, IReadOnlyDictionary<string, string?>? properties = null);
}

public interface IPanelDescriptorRegistry
{
    bool TryGetDescriptor(PanelTypeId typeId, out PanelDescriptor descriptor);

    IReadOnlyCollection<PanelDescriptor> GetAllDescriptors();
}
