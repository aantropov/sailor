using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Globalization;
using System.Collections;
using System.Reflection;

namespace SailorEditor.Utility;

public sealed record InspectorAggregateValueTarget(
    Func<object?> Read,
    Func<object?, CancellationToken, Task<bool>> Apply);

public sealed class InspectorAggregateField : ObservableObject, IInspectorEditable
{
    readonly IReadOnlyList<InspectorAggregateValueTarget> targets;
    readonly SemaphoreSlim commitGate = new(1, 1);
    object? draft;
    long editVersion;
    long committedVersion;
    int pendingCommits;
    bool hasInputText;
    string? inputText;

    public InspectorAggregateField(
        string name,
        Type valueType,
        IReadOnlyList<InspectorAggregateValueTarget> targets)
    {
        Name = name;
        ValueType = valueType;
        this.targets = targets;
    }

    public string Name { get; }
    public Type ValueType { get; }
    public string[] Choices { get; init; } = [];
    public NumericPropertyRange? Range { get; init; }
    public Type? SupportedAssetType { get; init; }
    public string ExpectedInstanceType { get; init; } = string.Empty;
    public bool IsReadOnly { get; init; }
    public IReadOnlyList<InspectorAggregateField> Children { get; init; } = [];
    public IReadOnlyList<InspectorAggregateAction> Actions { get; init; } = [];
    public Func<Func<Task<bool>>, CancellationToken, Task<bool>>? ApplyGroup { get; init; }
    public event Action? Changed;

    public object? Value => HasDraft ? draft : targets.FirstOrDefault()?.Read();
    bool HasDraft => editVersion != committedVersion;
    public bool IsMixed => !HasDraft && targets.Count > 1 &&
        targets.Skip(1).Any(target => !ValuesEqual(targets[0].Read(), target.Read()));
    public string DisplayValue => hasInputText ? inputText ?? string.Empty : IsMixed ? "*" : FormatValue(Value);
    public float FloatValue => Convert.ToSingle(Value ?? 0, CultureInfo.InvariantCulture);
    public int IntValue => Convert.ToInt32(Value ?? 0, CultureInfo.InvariantCulture);
    public uint UIntValue => Convert.ToUInt32(Value ?? 0, CultureInfo.InvariantCulture);
    public FileId FileIdValue
    {
        get => Value as FileId ?? new FileId();
        set => SetDraft(value);
    }
    public InstanceId InstanceIdValue
    {
        get => Value as InstanceId ?? new InstanceId();
        set => SetDraft(value);
    }
    public string ChoiceValue => IsMixed ? "*" : FormatValue(Value);

    public bool HasPendingInspectorChanges => hasInputText || HasDraft || HasInFlightInspectorCommit ||
        Children.Any(child => child.HasPendingInspectorChanges);
    public bool HasInFlightInspectorCommit => Volatile.Read(ref pendingCommits) != 0 ||
        Children.Any(child => child.HasInFlightInspectorCommit);

    public void SetDraft(object? value)
    {
        if (IsReadOnly || (!IsMixed && ValuesEqual(Value, value)))
        {
            return;
        }

        draft = CloneValue(value);
        ++editVersion;
        NotifyValueChanged();
    }

    public void SetInputText(string? text)
    {
        if (IsReadOnly)
        {
            return;
        }
        inputText = text;
        hasInputText = true;
        Changed?.Invoke();
    }

    public bool SetText(string? text)
    {
        if (IsReadOnly)
        {
            return false;
        }

        object? value;
        if (ValueType == typeof(string))
        {
            value = text ?? string.Empty;
        }
        else if (ValueType == typeof(float) && float.TryParse(text, NumberStyles.Float,
                     CultureInfo.InvariantCulture, out var number) && float.IsFinite(number))
        {
            value = Range is { } range ? range.Clamp(number) : number;
        }
        else if (ValueType == typeof(int) && int.TryParse(text, NumberStyles.Integer,
                     CultureInfo.InvariantCulture, out var integer))
        {
            value = Range is { } range ? range.Clamp(integer) : integer;
        }
        else if (ValueType == typeof(uint) && uint.TryParse(text, NumberStyles.Integer,
                     CultureInfo.InvariantCulture, out var unsigned))
        {
            value = Range is { } range ? range.Clamp(unsigned) : unsigned;
        }
        else
        {
            return false;
        }

        SetDraft(value);
        return true;
    }

    public void SetChoice(string value)
    {
        if (value == "*")
        {
            return;
        }

        SetDraft(ValueType == typeof(bool) ? bool.Parse(value) : value);
    }

    public async Task<bool> CommitInspectorChangesAsync(CancellationToken cancellationToken = default)
    {
        Interlocked.Increment(ref pendingCommits);
        var acquired = false;
        try
        {
            await commitGate.WaitAsync(cancellationToken);
            acquired = true;
            while (HasDraft || hasInputText)
            {
                if (hasInputText)
                {
                    var text = inputText;
                    hasInputText = false;
                    SetText(text);
                }
                if (!HasDraft)
                {
                    NotifyValueChanged();
                    continue;
                }
                var version = editVersion;
                var value = CloneValue(draft);
                async Task<bool> ApplyTargetsAsync()
                {
                    foreach (var target in targets)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        if (!await target.Apply(CloneValue(value), cancellationToken))
                        {
                            return false;
                        }
                    }
                    return true;
                }
                var succeeded = ApplyGroup is null
                    ? await ApplyTargetsAsync()
                    : await ApplyGroup(ApplyTargetsAsync, cancellationToken);
                if (!succeeded)
                {
                    return false;
                }

                committedVersion = version;
                NotifyValueChanged();
            }

            foreach (var child in Children)
            {
                if (!await child.CommitInspectorChangesAsync(cancellationToken))
                {
                    return false;
                }
            }
            return true;
        }
        finally
        {
            if (acquired)
            {
                commitGate.Release();
            }
            Interlocked.Decrement(ref pendingCommits);
            Changed?.Invoke();
        }
    }

    public void DiscardDrafts()
    {
        committedVersion = editVersion;
        hasInputText = false;
        foreach (var child in Children)
        {
            child.DiscardDrafts();
        }
        NotifyValueChanged();
    }

    void NotifyValueChanged()
    {
        OnPropertyChanged(nameof(Value));
        OnPropertyChanged(nameof(IsMixed));
        OnPropertyChanged(nameof(DisplayValue));
        OnPropertyChanged(nameof(ChoiceValue));
        OnPropertyChanged(nameof(FloatValue));
        OnPropertyChanged(nameof(IntValue));
        OnPropertyChanged(nameof(UIntValue));
        OnPropertyChanged(nameof(FileIdValue));
        OnPropertyChanged(nameof(InstanceIdValue));
        Changed?.Invoke();
    }

    public static object? CloneValue(object? value)
    {
        if (value is null || value is string || value.GetType().IsValueType)
        {
            return value;
        }
        if (value is ICloneable cloneable)
        {
            var clone = cloneable.Clone();
            if (clone is not null && value.GetType().IsInstanceOfType(clone))
            {
                return clone;
            }
            // Some reflected value subclasses inherit a Clone implementation
            // returning the base class; typed lists must retain the concrete type.
        }
        if (value is IList values)
        {
            var copy = (IList)Activator.CreateInstance(value.GetType())!;
            foreach (var item in values)
            {
                copy.Add(CloneValue(item));
            }
            return copy;
        }
        if (value is ObservableObject)
        {
            var copy = Activator.CreateInstance(value.GetType())!;
            foreach (var property in value.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance)
                         .Where(property => property.CanRead && property.GetIndexParameters().Length == 0))
            {
                if (property.SetMethod?.IsPublic == true)
                {
                    property.SetValue(copy, CloneValue(property.GetValue(value)));
                }
                else if (property.GetValue(value) is IList source && property.GetValue(copy) is IList target)
                {
                    target.Clear();
                    foreach (var item in source)
                    {
                        target.Add(CloneValue(item));
                    }
                }
            }
            return copy;
        }
        return value;
    }

    public static bool ValuesEqual(object? left, object? right)
    {
        if (ReferenceEquals(left, right))
        {
            return true;
        }
        if (left is null || right is null || left.GetType() != right.GetType())
        {
            return false;
        }
        if (left is ObservableRecord firstRecord && right is ObservableRecord secondRecord)
        {
            return firstRecord.Fields.Count() == secondRecord.Fields.Count() && firstRecord.Fields.All(field =>
                secondRecord.Fields.TryGetValue(field.Key, out var other) && ValuesEqual(field.Value, other));
        }
        if (left is ObservableValueList firstValues && right is ObservableValueList secondValues)
        {
            return ValuesEqual(firstValues.Values, secondValues.Values);
        }
        if (left is ObservableFileIdList firstIds && right is ObservableFileIdList secondIds)
        {
            return ValuesEqual(firstIds.Values, secondIds.Values);
        }
        if (left is ObservableFloatList firstFloats && right is ObservableFloatList secondFloats)
        {
            return ValuesEqual(firstFloats.Values, secondFloats.Values);
        }
        if (left is System.Collections.IEnumerable first && right is System.Collections.IEnumerable second &&
            left is not string)
        {
            return first.Cast<object?>().SequenceEqual(second.Cast<object?>(), AggregateValueComparer.Instance);
        }
        if (left is IComparable comparable)
        {
            return comparable.CompareTo(right) == 0;
        }
        var comparison = left.GetType().GetMethod("CompareTo", [left.GetType()]);
        if (comparison is null && left is ObservableObject)
        {
            return left.GetType().GetProperties().Where(property => property.CanRead && property.GetIndexParameters().Length == 0)
                .All(property => ValuesEqual(property.GetValue(left), property.GetValue(right)));
        }
        return comparison is not null ? (int)comparison.Invoke(left, [right])! == 0 : left.Equals(right);
    }

    static string FormatValue(object? value) => value switch
    {
        null => string.Empty,
        float number => number.ToString("G", CultureInfo.InvariantCulture),
        IFormattable formatted => formatted.ToString(null, CultureInfo.InvariantCulture),
        _ => value.ToString() ?? string.Empty
    };

    sealed class AggregateValueComparer : IEqualityComparer<object?>
    {
        public static AggregateValueComparer Instance { get; } = new();
        public new bool Equals(object? left, object? right) => ValuesEqual(left, right);
        public int GetHashCode(object? value) => 0;
    }
}

public sealed record InspectorAggregateAction(string Name, Func<CancellationToken, Task> Execute);

public sealed record InspectorAggregateSection(
    string Name,
    IReadOnlyList<InspectorAggregateField> Fields,
    IReadOnlyList<InspectorAggregateAction>? Actions = null,
    bool IsComponent = false);

public sealed class AggregateInspectorModel : ObservableObject, IInspectorEditable
{
    readonly Func<bool> isDirty;
    readonly Func<CancellationToken, Task<bool>>? save;
    readonly Func<CancellationToken, Task>? revert;

    public AggregateInspectorModel(
        int count,
        IReadOnlyList<InspectorAggregateSection> sections,
        bool isAssetSelection = false,
        Func<bool>? isDirty = null,
        Func<CancellationToken, Task<bool>>? save = null,
        Func<CancellationToken, Task>? revert = null)
    {
        Count = count;
        Sections = sections;
        IsAssetSelection = isAssetSelection;
        this.isDirty = isDirty ?? (() => false);
        this.save = save;
        this.revert = revert;
        foreach (var field in EnumerateFields(sections.SelectMany(section => section.Fields)))
        {
            field.Changed += NotifyDirtyChanged;
        }
    }

    public int Count { get; }
    public event Action? CommitStateChanged;
    public string DisplayName => $"{Count} selected";
    public bool IsAssetSelection { get; }
    public bool IsGameObjectSelection { get; init; }
    public bool IsDirty => isDirty() || Sections.SelectMany(section => section.Fields)
        .Any(property => property.HasPendingInspectorChanges);
    public IReadOnlyList<InspectorAggregateSection> Sections { get; }
    public bool HasPendingInspectorChanges => Sections.SelectMany(section => section.Fields)
        .Any(property => property.HasPendingInspectorChanges);
    public bool HasInFlightInspectorCommit => Sections.SelectMany(section => section.Fields)
        .Any(property => property.HasInFlightInspectorCommit);

    public async Task<bool> CommitInspectorChangesAsync(CancellationToken cancellationToken = default)
    {
        foreach (var field in Sections.SelectMany(section => section.Fields))
        {
            if (!await field.CommitInspectorChangesAsync(cancellationToken))
            {
                return false;
            }
        }
        NotifyDirtyChanged();
        return true;
    }

    public async Task<bool> SaveAsync(CancellationToken cancellationToken = default)
    {
        if (!await CommitInspectorChangesAsync(cancellationToken) || save is null)
        {
            return false;
        }
        var succeeded = await save(cancellationToken);
        NotifyDirtyChanged();
        return succeeded;
    }

    public async Task RevertAsync(CancellationToken cancellationToken = default)
    {
        foreach (var field in Sections.SelectMany(section => section.Fields))
        {
            field.DiscardDrafts();
        }
        if (revert is not null)
        {
            await revert(cancellationToken);
        }
        NotifyDirtyChanged();
    }

    void NotifyDirtyChanged()
    {
        OnPropertyChanged(nameof(IsDirty));
        CommitStateChanged?.Invoke();
    }

    static IEnumerable<InspectorAggregateField> EnumerateFields(IEnumerable<InspectorAggregateField> fields)
    {
        foreach (var field in fields)
        {
            yield return field;
            foreach (var child in EnumerateFields(field.Children))
            {
                yield return child;
            }
        }
    }
}

public sealed class AggregateGameObjectInspector : ObservableObject, IInspectorEditable
{
    readonly AggregateInspectorModel model;
    readonly InspectorAggregateField mobility;

    public AggregateGameObjectInspector(AggregateInspectorModel model)
    {
        this.model = model;
        var transform = model.Sections.Single(section => section.Name == "Transform");
        Position = new InspectorAggregateVectorBinding(transform.Fields.Single(property => property.Name == "Position"));
        Rotation = new InspectorAggregateVectorBinding(transform.Fields.Single(property => property.Name == "Rotation"));
        Scale = new InspectorAggregateVectorBinding(transform.Fields.Single(property => property.Name == "Scale"));
        mobility = transform.Fields.Single(property => property.Name == "MobilityType");
        mobility.Changed += () =>
        {
            OnPropertyChanged(nameof(MobilityType));
            OnPropertyChanged(nameof(MobilityTypes));
        };
    }

    public InspectorAggregateVectorBinding Position { get; }
    public InspectorAggregateVectorBinding Rotation { get; }
    public InspectorAggregateVectorBinding Scale { get; }
    public IReadOnlyList<InspectorAggregateSection> Components => model.Sections.Where(section => section.IsComponent).ToArray();
    public IList<string> MobilityTypes => mobility.IsMixed ? ["*", .. mobility.Choices] : mobility.Choices;
    public string MobilityType
    {
        get => mobility.ChoiceValue;
        set
        {
            if (string.Equals(value, mobility.ChoiceValue, StringComparison.Ordinal))
            {
                return;
            }
            mobility.SetChoice(value);
            if (mobility.HasPendingInspectorChanges)
            {
                _ = CommitMobilitySafelyAsync();
            }
        }
    }
    public bool HasPendingInspectorChanges => model.HasPendingInspectorChanges;
    public bool HasInFlightInspectorCommit => model.HasInFlightInspectorCommit;
    public Task<bool> CommitInspectorChangesAsync(CancellationToken cancellationToken = default)
        => model.CommitInspectorChangesAsync(cancellationToken);

    public async Task AddComponentFromInspectorAsync()
    {
        if (await model.CommitInspectorChangesAsync())
        {
            await model.Sections.Single(section => section.Name == "Components").Actions!
                .Single(action => action.Name == "Add Component").Execute(CancellationToken.None);
        }
    }

    async Task CommitMobilitySafelyAsync()
    {
        try
        {
            await model.CommitInspectorChangesAsync();
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"Aggregate mobility edit failed: {error}");
        }
    }
}

public sealed class InspectorAggregateVectorBinding : ObservableObject
{
    readonly IReadOnlyDictionary<string, InspectorAggregateField> properties;

    public InspectorAggregateVectorBinding(InspectorAggregateField vector)
    {
        properties = vector.Children.ToDictionary(property => property.Name, StringComparer.Ordinal);
        foreach (var property in properties.Values)
        {
            property.Changed += () => OnPropertyChanged(property.Name);
        }
    }

    public object X { get => Get(nameof(X)); set => Set(nameof(X), value); }
    public object Y { get => Get(nameof(Y)); set => Set(nameof(Y), value); }
    public object Z { get => Get(nameof(Z)); set => Set(nameof(Z), value); }
    public object W { get => Get(nameof(W)); set => Set(nameof(W), value); }
    public object Yaw { get => Get(nameof(Yaw)); set => Set(nameof(Yaw), value); }
    public object Pitch { get => Get(nameof(Pitch)); set => Set(nameof(Pitch), value); }
    public object Roll { get => Get(nameof(Roll)); set => Set(nameof(Roll), value); }

    object Get(string name) => properties[name];

    void Set(string name, object value)
    {
        if (value is float number && float.IsFinite(number))
        {
            properties[name].SetDraft(number);
        }
    }
}
