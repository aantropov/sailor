using CommunityToolkit.Mvvm.ComponentModel;
using SailorEngine;
using SailorEditor.Helpers;
using SailorEditor.Utility;
using System.ComponentModel;
using System.Globalization;
using System.Security.Cryptography;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.ViewModels;

public enum AnimationControllerParameterType
{
    Float,
    Int,
    Bool,
    Trigger
}

public enum AnimationControllerConditionOperation
{
    Equal,
    NotEqual,
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
    IsSet
}

public partial class AnimationControllerFile : AssetFile
{
    public event Action? DocumentReplaced;

    public ObservableList<AnimationControllerParameter> Parameters { get; private set; } = [];
    public ObservableList<AnimationControllerState> States { get; private set; } = [];
    public ObservableList<AnimationControllerTransition> Transitions { get; private set; } = [];

    public ulong DefaultStateId { get; private set; }

    public override Task Save()
    {
        EnsureWritable();
        ValidateDocument();
        if (!string.IsNullOrEmpty(LoadWarning))
        {
            throw new InvalidDataException(LoadWarning);
        }

        SaveControllerSource();
        return base.Save();
    }

    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();
                LoadControllerSource();
                DisplayName = Asset.Name;
                IsLoaded = false;
            });
        }
        catch (Exception exception)
        {
            SetLoadError(exception);
        }

        ResetDirtyState();
        DocumentReplaced?.Invoke();
        return Task.CompletedTask;
    }

    public string CaptureDocument()
    {
        var stream = new YamlStream(new YamlDocument(BuildControllerYaml()));
        using var writer = new StringWriter(CultureInfo.InvariantCulture);
        stream.Save(writer, false);
        return writer.ToString();
    }

    public bool ApplyDocument(string yaml, bool markDirty = true)
    {
        var stream = new YamlStream();
        using var reader = new StringReader(yaml ?? string.Empty);
        stream.Load(reader);
        if (stream.Documents.Count != 1 ||
            stream.Documents[0].RootNode is not YamlMappingNode root)
        {
            return false;
        }

        RunWithoutDirtyTracking(() => LoadControllerRoot(root));
        if (markDirty)
        {
            MarkDirty(nameof(States));
        }
        else
        {
            ResetDirtyState();
        }
        DocumentReplaced?.Invoke();
        return true;
    }

    public AnimationControllerState AddState(double x = 32.0, double y = 32.0)
    {
        var id = CreateStableId();
        var state = new AnimationControllerState
        {
            Id = id,
            Name = GetUniqueName("State", States.Select(item => item.Name)),
            Clip = "Animation",
            EditorX = (float)x,
            EditorY = (float)y
        };
        States.Add(state);
        if (DefaultStateId == 0)
        {
            DefaultStateId = id;
        }
        ValidateDocument();
        return state;
    }

    public bool RemoveState(AnimationControllerState? state)
    {
        if (state is null || !States.Remove(state))
        {
            return false;
        }

        foreach (var transition in Transitions
            .Where(item => item.FromStateId == state.Id || item.ToStateId == state.Id)
            .ToArray())
        {
            Transitions.Remove(transition);
        }
        if (DefaultStateId == state.Id)
        {
            DefaultStateId = States.FirstOrDefault()?.Id ?? 0;
        }
        ValidateDocument();
        return true;
    }

    public void SetDefaultState(AnimationControllerState? state)
    {
        if (state is not null && States.Contains(state))
        {
            DefaultStateId = state.Id;
            MarkDirty(nameof(DefaultStateId));
            ValidateDocument();
        }
    }

    public AnimationControllerParameter AddParameter(
        AnimationControllerParameterType type = AnimationControllerParameterType.Float)
    {
        var parameter = new AnimationControllerParameter
        {
            Id = CreateStableId(),
            Name = GetUniqueName("Parameter", Parameters.Select(item => item.Name)),
            Type = type
        };
        Parameters.Add(parameter);
        ValidateDocument();
        return parameter;
    }

    public bool RemoveParameter(AnimationControllerParameter? parameter)
    {
        if (parameter is null || !Parameters.Remove(parameter))
        {
            return false;
        }

        foreach (var transition in Transitions)
        {
            foreach (var condition in transition.Conditions
                .Where(item => item.ParameterId == parameter.Id)
                .ToArray())
            {
                transition.Conditions.Remove(condition);
            }
        }
        ValidateDocument();
        return true;
    }

    public AnimationControllerTransition? AddTransition(
        AnimationControllerState? from,
        AnimationControllerState? to)
    {
        if (from is null || to is null || !States.Contains(from) || !States.Contains(to))
        {
            return null;
        }

        var transition = new AnimationControllerTransition
        {
            Id = CreateStableId(),
            FromStateId = from.Id,
            ToStateId = to.Id
        };
        Transitions.Add(transition);
        ValidateDocument();
        return transition;
    }

    public bool RemoveTransition(AnimationControllerTransition? transition)
    {
        var removed = transition is not null && Transitions.Remove(transition);
        if (removed)
        {
            ValidateDocument();
        }
        return removed;
    }

    public AnimationControllerCondition? AddCondition(
        AnimationControllerTransition? transition,
        AnimationControllerParameter? parameter)
    {
        if (transition is null || parameter is null ||
            !Transitions.Contains(transition) || !Parameters.Contains(parameter))
        {
            return null;
        }

        var condition = new AnimationControllerCondition
        {
            ParameterId = parameter.Id,
            Operation = parameter.Type == AnimationControllerParameterType.Trigger
                ? AnimationControllerConditionOperation.IsSet
                : AnimationControllerConditionOperation.Equal
        };
        transition.Conditions.Add(condition);
        ValidateDocument();
        return condition;
    }

    public void ValidateDocument()
    {
        var errors = new List<string>();
        if (States.Count == 0)
        {
            errors.Add("The controller must contain at least one state.");
        }
        if (States.All(state => state.Id != DefaultStateId))
        {
            errors.Add("Default state is missing.");
        }
        AddDuplicateErrors(
            Parameters.Select(parameter => parameter.Id),
            "Parameter stable IDs must be non-zero and unique.",
            errors);
        AddDuplicateErrors(
            Parameters.Select(parameter => parameter.Name),
            "Parameter names must be non-empty and unique.",
            errors);
        AddDuplicateErrors(
            States.Select(state => state.Id),
            "State stable IDs must be non-zero and unique.",
            errors);
        AddDuplicateErrors(
            States.Select(state => state.Name),
            "State names must be non-empty and unique.",
            errors);
        AddDuplicateErrors(
            Transitions.Select(transition => transition.Id),
            "Transition stable IDs must be non-zero and unique.",
            errors);

        var stateIds = States.Select(state => state.Id).ToHashSet();
        var parameters = new Dictionary<ulong, AnimationControllerParameter>();
        foreach (var parameter in Parameters)
        {
            parameters.TryAdd(parameter.Id, parameter);
        }
        foreach (var state in States)
        {
            if (string.IsNullOrWhiteSpace(state.Clip) || state.Speed <= 0.0f ||
                !float.IsFinite(state.Speed) || !float.IsFinite(state.EditorX) ||
                !float.IsFinite(state.EditorY))
            {
                errors.Add($"State '{state.Name}' requires a clip slot and finite positive speed/position.");
            }
        }
        foreach (var transition in Transitions)
        {
            if (!stateIds.Contains(transition.FromStateId) ||
                !stateIds.Contains(transition.ToStateId) ||
                transition.Duration < 0.0f || !float.IsFinite(transition.Duration) ||
                transition.ExitTime is < 0.0f or > 1.0f || !float.IsFinite(transition.ExitTime))
            {
                errors.Add($"Transition {transition.Id} contains invalid states or timing.");
            }
            foreach (var condition in transition.Conditions)
            {
                if (!parameters.TryGetValue(condition.ParameterId, out var parameter) ||
                    !IsOperationValid(parameter.Type, condition.Operation))
                {
                    errors.Add($"Transition {transition.Id} contains an invalid typed condition.");
                }
            }
        }

        LoadWarning = string.Join(Environment.NewLine, errors.Distinct());
    }

    void LoadControllerSource()
    {
        var stream = new YamlStream();
        using var reader = new StreamReader(Asset.FullName);
        stream.Load(reader);
        if (stream.Documents.Count != 1 ||
            stream.Documents[0].RootNode is not YamlMappingNode root)
        {
            throw new InvalidDataException($"Animation controller must contain one YAML mapping: {Asset.FullName}");
        }
        LoadControllerRoot(root);
    }

    void LoadControllerRoot(YamlMappingNode root)
    {
        var defaultStateId = YamlHelper.ReadUInt64(root, "defaultState");
        var parameters = ReadList(root, "parameters", AnimationControllerParameter.FromYaml);
        var states = ReadList(root, "states", AnimationControllerState.FromYaml);
        var transitions = ReadList(root, "transitions", AnimationControllerTransition.FromYaml);

        DefaultStateId = defaultStateId;
        Parameters = parameters;
        States = states;
        Transitions = transitions;
        TrackList(Parameters, nameof(Parameters));
        TrackList(States, nameof(States));
        TrackList(Transitions, nameof(Transitions));
        ValidateDocument();
        OnPropertyChanged(nameof(Parameters));
        OnPropertyChanged(nameof(States));
        OnPropertyChanged(nameof(Transitions));
        OnPropertyChanged(nameof(DefaultStateId));
    }

    void SaveControllerSource()
    {
        var stream = new YamlStream(new YamlDocument(BuildControllerYaml()));
        using var writer = new StreamWriter(new FileStream(Asset.FullName, FileMode.Create));
        stream.Save(writer, false);
    }

    YamlMappingNode BuildControllerYaml() => new()
    {
        { "version", "1" },
        { "defaultState", DefaultStateId.ToString(CultureInfo.InvariantCulture) },
        { "parameters", WriteList(Parameters, item => item.ToYaml()) },
        { "states", WriteList(States, item => item.ToYaml()) },
        { "transitions", WriteList(Transitions, item => item.ToYaml()) }
    };

    void TrackList<T>(ObservableList<T> list, string propertyName)
        where T : INotifyPropertyChanged
    {
        list.CollectionChanged += (_, _) =>
        {
            MarkDirty(propertyName);
            ValidateDocument();
        };
        list.ItemChanged += (_, _) =>
        {
            MarkDirty(propertyName);
            ValidateDocument();
        };
    }

    ulong CreateStableId()
    {
        var existing = Parameters.Select(item => item.Id)
            .Concat(States.Select(item => item.Id))
            .Concat(Transitions.Select(item => item.Id))
            .ToHashSet();
        ulong id;
        do
        {
            id = BitConverter.ToUInt64(
                RandomNumberGenerator.GetBytes(sizeof(ulong)));
        } while (id == 0 || existing.Contains(id));
        return id;
    }

    static ObservableList<T> ReadList<T>(
        YamlMappingNode root,
        string key,
        Func<YamlMappingNode, T> factory)
        where T : INotifyPropertyChanged
    {
        var result = new ObservableList<T>();
        if (YamlHelper.TryGetSequence(root, key, out var sequence))
        {
            foreach (var node in sequence.Children.OfType<YamlMappingNode>())
            {
                result.Add(factory(node));
            }
        }
        return result;
    }

    static YamlSequenceNode WriteList<T>(
        IEnumerable<T> values,
        Func<T, YamlMappingNode> serialize)
    {
        var result = new YamlSequenceNode();
        foreach (var value in values)
        {
            result.Add(serialize(value));
        }
        return result;
    }

    static void AddDuplicateErrors<T>(
        IEnumerable<T> values,
        string message,
        ICollection<string> errors)
    {
        var materialized = values.ToArray();
        if (materialized.Any(value => value is null ||
                value is string text && string.IsNullOrWhiteSpace(text) ||
                value is ulong id && id == 0) ||
            materialized.Distinct().Count() != materialized.Length)
        {
            errors.Add(message);
        }
    }

    static bool IsOperationValid(
        AnimationControllerParameterType type,
        AnimationControllerConditionOperation operation) => type switch
    {
        AnimationControllerParameterType.Trigger => operation == AnimationControllerConditionOperation.IsSet,
        AnimationControllerParameterType.Bool => operation is AnimationControllerConditionOperation.Equal or AnimationControllerConditionOperation.NotEqual,
        _ => operation != AnimationControllerConditionOperation.IsSet
    };

    static string GetUniqueName(string prefix, IEnumerable<string> existing)
    {
        var names = existing.ToHashSet(StringComparer.Ordinal);
        if (!names.Contains(prefix))
        {
            return prefix;
        }
        for (var index = 2; ; ++index)
        {
            var candidate = $"{prefix} {index}";
            if (!names.Contains(candidate))
            {
                return candidate;
            }
        }
    }

}

public partial class AnimationControllerParameter : ObservableObject
{
    [ObservableProperty]
    ulong id;

    [ObservableProperty]
    string name = string.Empty;

    [ObservableProperty]
    AnimationControllerParameterType type;

    [ObservableProperty]
    float defaultFloat;

    [ObservableProperty]
    int defaultInt;

    [ObservableProperty]
    bool defaultBool;

    public static AnimationControllerParameter FromYaml(YamlMappingNode node)
    {
        var serializedType = YamlHelper.ReadString(node, "type", "Float");
        if (!Enum.TryParse<AnimationControllerParameterType>(
                serializedType,
                ignoreCase: false,
                out var type) ||
            !Enum.IsDefined(type))
        {
            throw new InvalidDataException(
                $"Unknown animation parameter type '{serializedType}'.");
        }
        var result = new AnimationControllerParameter
        {
            Id = YamlHelper.ReadUInt64(node, "id"),
            Name = YamlHelper.ReadString(node, "name"),
            Type = type
        };
        switch (type)
        {
            case AnimationControllerParameterType.Float:
                result.DefaultFloat = YamlHelper.ReadFloat(node, "default");
                break;
            case AnimationControllerParameterType.Int:
                result.DefaultInt = YamlHelper.ReadInt(node, "default");
                break;
            case AnimationControllerParameterType.Bool:
                result.DefaultBool = YamlHelper.ReadBool(node, "default");
                break;
        }
        return result;
    }

    public YamlMappingNode ToYaml()
    {
        var result = new YamlMappingNode
        {
            { "id", YamlHelper.Scalar(Id) },
            { "name", Name ?? string.Empty },
            { "type", Type.ToString() }
        };
        switch (Type)
        {
            case AnimationControllerParameterType.Float:
                result.Add("default", YamlHelper.Scalar(DefaultFloat));
                break;
            case AnimationControllerParameterType.Int:
                result.Add("default", YamlHelper.Scalar(DefaultInt));
                break;
            case AnimationControllerParameterType.Bool:
                result.Add("default", YamlHelper.Scalar(DefaultBool));
                break;
        }
        return result;
    }
}

public partial class AnimationControllerState : ObservableObject
{
    [ObservableProperty]
    ulong id;

    [ObservableProperty]
    string name = string.Empty;

    [ObservableProperty]
    string clip = string.Empty;

    [ObservableProperty]
    float speed = 1.0f;

    [ObservableProperty]
    bool loop = true;

    [ObservableProperty]
    float editorX;

    [ObservableProperty]
    float editorY;

    public static AnimationControllerState FromYaml(YamlMappingNode node)
    {
        var editor = YamlHelper.TryGetMapping(node, "editor", out var editorMapping)
            ? editorMapping
            : null;
        return new AnimationControllerState
        {
            Id = YamlHelper.ReadUInt64(node, "id"),
            Name = YamlHelper.ReadString(node, "name"),
            Clip = YamlHelper.ReadString(node, "clip"),
            Speed = YamlHelper.ReadFloat(node, "speed", 1.0f),
            Loop = YamlHelper.ReadBool(node, "loop", true),
            EditorX = editor is null ? 0.0f : YamlHelper.ReadFloat(editor, "x"),
            EditorY = editor is null ? 0.0f : YamlHelper.ReadFloat(editor, "y")
        };
    }

    public YamlMappingNode ToYaml() => new()
    {
        { "id", YamlHelper.Scalar(Id) },
        { "name", Name ?? string.Empty },
        { "clip", Clip ?? string.Empty },
        { "speed", YamlHelper.Scalar(Speed) },
        { "loop", YamlHelper.Scalar(Loop) },
        {
            "editor",
            new YamlMappingNode
            {
                { "x", YamlHelper.Scalar(EditorX) },
                { "y", YamlHelper.Scalar(EditorY) }
            }
        }
    };
}

public partial class AnimationControllerCondition : ObservableObject
{
    [ObservableProperty]
    ulong parameterId;

    [ObservableProperty]
    AnimationControllerConditionOperation operation;

    [ObservableProperty]
    float floatValue;

    [ObservableProperty]
    int intValue;

    [ObservableProperty]
    bool boolValue;

    public static AnimationControllerCondition FromYaml(YamlMappingNode node)
    {
        var serializedOperation = YamlHelper.ReadString(
            node,
            "operation",
            "Equal");
        if (!Enum.TryParse<AnimationControllerConditionOperation>(
                serializedOperation,
                ignoreCase: false,
                out var operation) ||
            !Enum.IsDefined(operation))
        {
            throw new InvalidDataException(
                $"Unknown animation condition operation '{serializedOperation}'.");
        }
        return new AnimationControllerCondition
        {
            ParameterId = YamlHelper.ReadUInt64(node, "parameter"),
            Operation = operation,
            FloatValue = YamlHelper.ReadFloat(node, "floatValue"),
            IntValue = YamlHelper.ReadInt(node, "intValue"),
            BoolValue = YamlHelper.ReadBool(node, "boolValue")
        };
    }

    public YamlMappingNode ToYaml() => new()
    {
        { "parameter", YamlHelper.Scalar(ParameterId) },
        { "operation", Operation.ToString() },
        { "floatValue", YamlHelper.Scalar(FloatValue) },
        { "intValue", YamlHelper.Scalar(IntValue) },
        { "boolValue", YamlHelper.Scalar(BoolValue) }
    };
}

public partial class AnimationControllerTransition : ObservableObject
{
    public AnimationControllerTransition()
    {
        Conditions.CollectionChanged += (_, _) => OnPropertyChanged(nameof(Conditions));
        Conditions.ItemChanged += (_, _) => OnPropertyChanged(nameof(Conditions));
    }

    [ObservableProperty]
    ulong id;

    [ObservableProperty]
    ulong fromStateId;

    [ObservableProperty]
    ulong toStateId;

    [ObservableProperty]
    int priority;

    [ObservableProperty]
    float duration = 0.2f;

    [ObservableProperty]
    bool hasExitTime;

    [ObservableProperty]
    float exitTime;

    public ObservableList<AnimationControllerCondition> Conditions { get; } = [];

    public static AnimationControllerTransition FromYaml(YamlMappingNode node)
    {
        var result = new AnimationControllerTransition
        {
            Id = YamlHelper.ReadUInt64(node, "id"),
            FromStateId = YamlHelper.ReadUInt64(node, "from"),
            ToStateId = YamlHelper.ReadUInt64(node, "to"),
            Priority = YamlHelper.ReadInt(node, "priority"),
            Duration = YamlHelper.ReadFloat(node, "duration", 0.2f),
            HasExitTime = YamlHelper.ReadBool(node, "hasExitTime"),
            ExitTime = YamlHelper.ReadFloat(node, "exitTime")
        };
        if (YamlHelper.TryGetSequence(node, "conditions", out var sequence))
        {
            foreach (var condition in sequence.Children.OfType<YamlMappingNode>())
            {
                result.Conditions.Add(AnimationControllerCondition.FromYaml(condition));
            }
        }
        return result;
    }

    public YamlMappingNode ToYaml()
    {
        var conditions = new YamlSequenceNode();
        foreach (var condition in Conditions)
        {
            conditions.Add(condition.ToYaml());
        }
        return new YamlMappingNode
        {
            { "id", YamlHelper.Scalar(Id) },
            { "from", YamlHelper.Scalar(FromStateId) },
            { "to", YamlHelper.Scalar(ToStateId) },
            { "priority", YamlHelper.Scalar(Priority) },
            { "duration", YamlHelper.Scalar(Duration) },
            { "hasExitTime", YamlHelper.Scalar(HasExitTime) },
            { "exitTime", YamlHelper.Scalar(ExitTime) },
            { "conditions", conditions }
        };
    }
}

public partial class AnimationSetFile : AssetFile
{
    public ObservableList<AnimationSetClip> Clips { get; private set; } = [];

    public override Task Save()
    {
        EnsureWritable();
        var duplicateSlots = Clips
            .GroupBy(clip => clip.Slot, StringComparer.Ordinal)
            .Any(group => string.IsNullOrWhiteSpace(group.Key) || group.Count() > 1);
        if (duplicateSlots || Clips.Any(clip => clip.Animation is null || clip.Animation.IsEmpty()))
        {
            throw new InvalidDataException("Animation set slots must be unique and reference an animation FileId.");
        }
        SaveSetSource();
        return base.Save();
    }

    public override Task Revert()
    {
        try
        {
            RunWithoutDirtyTracking(() =>
            {
                LoadAssetPropertiesFromAssetInfo();
                LoadSetSource();
                DisplayName = Asset.Name;
                IsLoaded = false;
            });
        }
        catch (Exception exception)
        {
            SetLoadError(exception);
        }
        ResetDirtyState();
        return Task.CompletedTask;
    }

    public AnimationSetClip AddClip()
    {
        var clip = new AnimationSetClip { Slot = "Animation" };
        Clips.Add(clip);
        return clip;
    }

    void LoadSetSource()
    {
        var stream = new YamlStream();
        using var reader = new StreamReader(Asset.FullName);
        stream.Load(reader);
        var root = stream.Documents.Count == 1
            ? stream.Documents[0].RootNode as YamlMappingNode
            : null;
        if (root is null)
        {
            throw new InvalidDataException($"Animation set must contain one YAML mapping: {Asset.FullName}");
        }

        Clips = [];
        if (YamlHelper.TryGetSequence(root, "clips", out var sequence))
        {
            foreach (var node in sequence.Children.OfType<YamlMappingNode>())
            {
                Clips.Add(new AnimationSetClip
                {
                    Slot = YamlHelper.ReadString(node, "slot"),
                    Animation = new FileId(YamlHelper.ReadString(node, "animation"))
                });
            }
        }
        Clips.CollectionChanged += (_, _) => MarkDirty(nameof(Clips));
        Clips.ItemChanged += (_, _) => MarkDirty(nameof(Clips));
        OnPropertyChanged(nameof(Clips));
    }

    void SaveSetSource()
    {
        var clips = new YamlSequenceNode();
        foreach (var clip in Clips)
        {
            clips.Add(new YamlMappingNode
            {
                { "slot", clip.Slot ?? string.Empty },
                { "animation", clip.Animation?.Value ?? FileId.NullFileId }
            });
        }
        var root = new YamlMappingNode
        {
            { "version", "1" },
            { "clips", clips }
        };
        var stream = new YamlStream(new YamlDocument(root));
        using var writer = new StreamWriter(new FileStream(Asset.FullName, FileMode.Create));
        stream.Save(writer, false);
    }
}

public partial class AnimationSetClip : ObservableObject
{
    [ObservableProperty]
    string slot = string.Empty;

    [ObservableProperty]
    FileId animation = new();
}
