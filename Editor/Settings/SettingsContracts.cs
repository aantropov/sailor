namespace SailorEditor.Settings;

public enum SettingsScope
{
    Project,
    Editor,
    Engine,
    AI,
}

public sealed record SettingsCategory(
    string Id,
    string DisplayName,
    IReadOnlyList<SettingsCategory>? Children = null,
    IReadOnlyList<SettingsEntry>? Entries = null);

public sealed record SettingsEntry(
    string Key,
    string DisplayName,
    SettingsValueKind ValueKind,
    SettingsScope Scope,
    string? Description = null,
    bool RequiresRestart = false,
    IReadOnlyList<string>? AllowedValues = null,
    bool IsSecret = false);

public enum SettingsValueKind
{
    Boolean,
    Integer,
    Float,
    String,
    Enum,
    Path,
}

public sealed record SettingsValidationMessage(string Key, string Message, SettingsValidationSeverity Severity);

public enum SettingsValidationSeverity
{
    Info,
    Warning,
    Error,
}

public sealed record SettingsValidationResult(IReadOnlyList<SettingsValidationMessage> Messages)
{
    public static SettingsValidationResult Success { get; } = new([]);

    public bool IsValid => Messages.All(message => message.Severity != SettingsValidationSeverity.Error);
}

public interface ISettingsValidator
{
    SettingsValidationResult Validate(SettingsEntry entry, object? value);
}

public interface ISettingsStore
{
    ValueTask<object?> GetValueAsync(SettingsEntry entry, CancellationToken cancellationToken = default);

    ValueTask SetValueAsync(SettingsEntry entry, object? value, CancellationToken cancellationToken = default);
}
