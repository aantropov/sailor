using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.Settings;

public sealed record SettingsDefinition(
    SettingsEntry Entry,
    object? DefaultValue = null,
    Func<object?, SettingsValidationResult>? Validator = null,
    IReadOnlyList<SettingsScope>? ResolutionOrder = null);

public sealed record EffectiveSettingsValue(SettingsDefinition Definition, object? Value, SettingsScope? SourceScope, bool IsDefault);

public sealed class DelegateSettingsValidator : ISettingsValidator
{
    public SettingsValidationResult Validate(SettingsEntry entry, object? value) => SettingsValidationResult.Success;
}

public sealed class UnifiedSettingsStore : ISettingsStore
{
    readonly IReadOnlyDictionary<string, SettingsDefinition> _definitions;
    readonly Dictionary<SettingsScope, Dictionary<string, object?>> _values = Enum.GetValues<SettingsScope>()
        .ToDictionary(scope => scope, _ => new Dictionary<string, object?>());

    public UnifiedSettingsStore(IEnumerable<SettingsDefinition> definitions)
    {
        _definitions = definitions.ToDictionary(x => x.Entry.Key, StringComparer.OrdinalIgnoreCase);
    }

    public IReadOnlyCollection<SettingsDefinition> Definitions => _definitions.Values.ToArray();

    public ValueTask<object?> GetValueAsync(SettingsEntry entry, CancellationToken cancellationToken = default)
        => ValueTask.FromResult(Resolve(entry).Value);

    public ValueTask SetValueAsync(SettingsEntry entry, object? value, CancellationToken cancellationToken = default)
        => SetValueAsync(entry, entry.Scope, value, cancellationToken);

    public ValueTask SetValueAsync(SettingsEntry entry, SettingsScope scope, object? value, CancellationToken cancellationToken = default)
    {
        var result = Validate(entry, value);
        if (!result.IsValid)
            throw new InvalidOperationException(string.Join(Environment.NewLine, result.Messages.Select(x => x.Message)));

        _values[scope][entry.Key] = value;
        return ValueTask.CompletedTask;
    }

    public EffectiveSettingsValue Resolve(SettingsEntry entry)
    {
        var definition = GetDefinition(entry.Key);
        foreach (var scope in definition.ResolutionOrder ?? DefaultResolutionOrder())
        {
            if (_values[scope].TryGetValue(entry.Key, out var value))
                return new EffectiveSettingsValue(definition, value, scope, false);
        }

        return new EffectiveSettingsValue(definition, definition.DefaultValue, null, true);
    }

    public SettingsValidationResult Validate(SettingsEntry entry, object? value)
    {
        var definition = GetDefinition(entry.Key);
        var messages = new List<SettingsValidationMessage>();

        if (definition.Entry.AllowedValues is { Count: > 0 } allowedValues && value is string stringValue && !allowedValues.Contains(stringValue))
        {
            messages.Add(new SettingsValidationMessage(entry.Key, $"'{stringValue}' is not an allowed value.", SettingsValidationSeverity.Error));
        }

        if (!MatchesKind(entry.ValueKind, value))
            messages.Add(new SettingsValidationMessage(entry.Key, $"Value does not match {entry.ValueKind}.", SettingsValidationSeverity.Error));

        var custom = definition.Validator?.Invoke(value);
        if (custom is not null)
            messages.AddRange(custom.Messages);

        return messages.Count == 0 ? SettingsValidationResult.Success : new SettingsValidationResult(messages);
    }

    public async Task SaveAsync(string path, CancellationToken cancellationToken = default)
    {
        var serializer = new SerializerBuilder().WithNamingConvention(CamelCaseNamingConvention.Instance).Build();
        var model = _values.ToDictionary(
            x => x.Key.ToString().ToLowerInvariant(),
            x => (IReadOnlyDictionary<string, object?>)new Dictionary<string, object?>(x.Value));
        await File.WriteAllTextAsync(path, serializer.Serialize(model), cancellationToken);
    }

    public async Task LoadAsync(string path, CancellationToken cancellationToken = default)
    {
        if (!File.Exists(path))
            return;

        var deserializer = new DeserializerBuilder().WithNamingConvention(CamelCaseNamingConvention.Instance).Build();
        var yaml = await File.ReadAllTextAsync(path, cancellationToken);
        var model = deserializer.Deserialize<Dictionary<string, Dictionary<string, object?>>>(yaml) ?? [];

        foreach (var (scopeKey, values) in model)
        {
            if (!Enum.TryParse<SettingsScope>(scopeKey, true, out var scope))
                continue;

            _values[scope].Clear();
            foreach (var pair in values)
                _values[scope][pair.Key] = pair.Value;
        }
    }

    SettingsDefinition GetDefinition(string key) => _definitions.TryGetValue(key, out var definition)
        ? definition
        : throw new KeyNotFoundException($"Unknown settings key '{key}'.");

    static bool MatchesKind(SettingsValueKind kind, object? value) => value switch
    {
        null => true,
        bool when kind == SettingsValueKind.Boolean => true,
        int when kind == SettingsValueKind.Integer => true,
        long when kind == SettingsValueKind.Integer => true,
        float when kind == SettingsValueKind.Float => true,
        double when kind == SettingsValueKind.Float => true,
        string when kind is SettingsValueKind.String or SettingsValueKind.Enum or SettingsValueKind.Path => true,
        _ => false,
    };

    static IReadOnlyList<SettingsScope> DefaultResolutionOrder() =>
        [SettingsScope.Project, SettingsScope.Editor, SettingsScope.Engine];
}

public static class EditorSettingsCatalog
{
    public static readonly SettingsDefinition[] Definitions =
    [
        new(
            new SettingsEntry("project.defaultWorld", "Default World", SettingsValueKind.Path, SettingsScope.Project, "Default startup world."),
            DefaultValue: "Content/Worlds/Main.world"),
        new(
            new SettingsEntry("editor.theme", "Theme", SettingsValueKind.Enum, SettingsScope.Editor, "Editor appearance.", AllowedValues: ["Dark", "Light"]),
            DefaultValue: "Dark"),
        new(
            new SettingsEntry("engine.vsync", "VSync", SettingsValueKind.Boolean, SettingsScope.Engine, "Toggle runtime vertical sync."),
            DefaultValue: true),
        new(
            new SettingsEntry("engine.renderScale", "Render Scale", SettingsValueKind.Float, SettingsScope.Engine, "Viewport render scale."),
            DefaultValue: 1.0,
            Validator: value => value is double d && d >= 0.25 && d <= 2.0
                ? SettingsValidationResult.Success
                : new SettingsValidationResult([new SettingsValidationMessage("engine.renderScale", "Render Scale must be between 0.25 and 2.0.", SettingsValidationSeverity.Error)]))
    ];

    public static SettingsCategory CreateCategoryTree() => new(
        "root",
        "Settings",
        Children:
        [
            new SettingsCategory("project", "Project", Entries: Definitions.Where(x => x.Entry.Scope == SettingsScope.Project).Select(x => x.Entry).ToArray()),
            new SettingsCategory("editor", "Editor", Entries: Definitions.Where(x => x.Entry.Scope == SettingsScope.Editor).Select(x => x.Entry).ToArray()),
            new SettingsCategory("engine", "Engine", Entries: Definitions.Where(x => x.Entry.Scope == SettingsScope.Engine).Select(x => x.Entry).ToArray()),
        ]);
}
