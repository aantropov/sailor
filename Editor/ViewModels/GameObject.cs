using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SailorEditor.Commands;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using System.Runtime.CompilerServices;
using SailorEditor.Services;
using SailorEditor.Workflow;
using System.Windows.Input;
using SailorEngine;

namespace SailorEditor.ViewModels;

public interface IInspectorEditable
{
    bool HasPendingInspectorChanges { get; }
    Task<bool> CommitInspectorChangesAsync(
        CancellationToken cancellationToken = default);
}

public partial class GameObject : ObservableObject, ICloneable, IInspectorEditable
{
    readonly InspectorAutoCommitController _autoCommit = new(
        propertyName => propertyName == nameof(IsDirty),
        propertyName => propertyName == nameof(MobilityType));
    readonly SemaphoreSlim _commitGate = new(1, 1);
    int pendingInspectorCommits;

    public GameObject()
    {
        AddNewComponent = new AsyncRelayCommand(AddComponentFromInspectorAsync);
        ClearComponentsCommand =
            new AsyncRelayCommand(ClearComponentsFromInspectorAsync);

        PropertyChanged += (s, args) =>
        {
            var decision = _autoCommit.OnPropertyChanged(args.PropertyName);
            if (!decision.MarkDirty)
                return;

            IsDirty = true;
            if (decision.CommitNow)
                _ = CommitInspectorChangesSafelyAsync();
        };
    }

    public async Task<bool> CommitInspectorChangesAsync(
        CancellationToken cancellationToken = default)
    {
        Interlocked.Increment(ref pendingInspectorCommits);
        var acquired = false;
        try
        {
            await _commitGate.WaitAsync(cancellationToken);
            acquired = true;
            if (!await CommitInspectorChangesCoreAsync(cancellationToken) &&
                HasPendingGameObjectChanges)
            {
                return false;
            }

            foreach (var component in GetComponentsSafely().ToArray())
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!component.HasPendingInspectorChanges)
                    continue;

                if (!await component.CommitInspectorChangesAsync(
                        cancellationToken) &&
                    component.HasPendingInspectorChanges)
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
                _commitGate.Release();
            }
            Interlocked.Decrement(ref pendingInspectorCommits);
        }
    }

    async Task<bool> CommitInspectorChangesCoreAsync(
        CancellationToken cancellationToken)
    {
        if (!isInited)
            return false;
        if (!_autoCommit.ShouldCommitPendingChanges(IsDirty))
            return true;

        var yamlGameObject = EditorYaml.SerializeGameObject(this);
        var previousYaml = _lastCommittedYaml ?? yamlGameObject;
        if (string.Equals(previousYaml, yamlGameObject, StringComparison.Ordinal))
        {
            IsDirty = false;
            return false;
        }

        var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
        var contextProvider = MauiProgram.GetService<IActionContextProvider>();
        IsDirty = false;

        CommandResult result;
        try
        {
            result = await dispatcher.DispatchAsync(
                new UpdateGameObjectCommand(this, previousYaml, yamlGameObject, $"Edit {Name}"),
                contextProvider.GetCurrentContext(
                    new CommandOrigin(
                        CommandOriginKind.UI,
                        nameof(CommitInspectorChangesAsync))),
                cancellationToken);
        }
        catch
        {
            IsDirty = true;
            throw;
        }

        if (result.Succeeded)
        {
            _lastCommittedYaml = yamlGameObject;
            return true;
        }

        IsDirty = true;
        return false;
    }

    async Task CommitInspectorChangesSafelyAsync()
    {
        try
        {
            await CommitInspectorChangesAsync();
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                $"Automatic GameObject inspector commit failed: {exception}");
        }
    }

    [YamlIgnore]
    public bool HasPendingInspectorChanges =>
        HasPendingGameObjectChanges ||
        GetComponentsSafely().Any(component =>
            component.HasPendingInspectorChanges);

    [YamlIgnore]
    bool HasPendingGameObjectChanges =>
        IsDirty || Volatile.Read(ref pendingInspectorCommits) != 0;

    IEnumerable<Component> GetComponentsSafely()
    {
        try
        {
            return Components;
        }
        catch
        {
            return [];
        }
    }

    public void Initialize()
    {
        InstanceId ??= InstanceId.NullInstanceId;
        Position ??= new Vec4();
        Rotation ??= new Rotation();
        Scale ??= new Vec4();
        ComponentIndices ??= [];
        MobilityType = GameObjectMobilityPolicy.Normalize(MobilityType);

        Scale.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Scale));
        Position.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Position));
        Rotation.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Rotation));

        IsDirty = false;
        _lastCommittedYaml = EditorYaml.SerializeGameObject(this);
        isInited = true;
        _autoCommit.MarkInitialized();
    }

    [YamlIgnore]
    protected bool isInited = false;

    [YamlIgnore]
    public string DisplayName { get { return Name; } }

    [YamlIgnore]
    public IList<string> MobilityTypes => GameObjectMobilityPolicy.Values;

    [YamlIgnore]
    public int PrefabIndex = -1;

    [YamlIgnore]
    public bool IsPrefabLinked => TryGetLinkedPrefab(out _);

    [YamlIgnore]
    public string? PrefabFileId =>
        TryGetLinkedPrefab(out var prefab)
            ? prefab.FileId?.Value
            : null;

    bool TryGetLinkedPrefab(out Prefab prefab)
    {
        prefab = null!;
        try
        {
            var world = MauiProgram.GetService<WorldService>();
            if (PrefabIndex < 0 ||
                PrefabIndex >= world.Current.Prefabs.Count)
            {
                return false;
            }

            prefab = world.Current.Prefabs[PrefabIndex];
            return prefab.FileId is not null &&
                !prefab.FileId.IsEmpty();
        }
        catch
        {
            return false;
        }
    }

    public void MarkDirty([CallerMemberName] string propertyName = null) { IsDirty = true; OnPropertyChanged(propertyName); }

    [YamlIgnore]
    public List<Component> Components { get { return MauiProgram.GetService<WorldService>().GetComponents(this); } }

    public void NotifyComponentsChanged()
        => OnPropertyChanged(nameof(Components));

    public object Clone() => new GameObject()
    {
        Name = Name + "(Clone)",
        Position = new Vec4(Position),
        Rotation = new Rotation(Rotation),
        Scale = new Vec4(Scale),
        MobilityType = MobilityType,
        ParentIndex = ParentIndex,
        InstanceId = InstanceId,
        ComponentIndices = new List<int>(ComponentIndices),
        IsDirty = false
    };

    [YamlIgnore]
    public ICommand AddNewComponent { get; }

    [YamlIgnore]
    public ICommand ClearComponentsCommand { get; }

    public async Task AddComponentFromInspectorAsync()
    {
        var editorTypes = MauiProgram.GetService<EngineService>().EngineTypes;
        var componentTypeName = await Views.AddComponentDialogPage.ShowAsync(
            editorTypes.GetAddableComponentTypeNames());

        if (!string.IsNullOrWhiteSpace(componentTypeName))
        {
            await MauiProgram.GetService<WorldService>()
                .AddComponentAsync(this, componentTypeName);
        }
    }

    public async Task ClearComponentsFromInspectorAsync()
    {
        var components = Components.ToList();
        foreach (var component in components)
        {
            await MauiProgram.GetService<WorldService>()
                .RemoveComponentAsync(component);
        }
    }

    [ObservableProperty]
    string name = string.Empty;

    partial void OnNameChanged(string value)
    {
        OnPropertyChanged(nameof(DisplayName));
    }

    [ObservableProperty]
    InstanceId instanceId = InstanceId.NullInstanceId;

    [ObservableProperty]
    string mobilityType = GameObjectMobilityPolicy.Stationary;

    [ObservableProperty]
    uint parentIndex = uint.MaxValue;

    [ObservableProperty]
    Vec4 position = new();

    [ObservableProperty]
    Rotation rotation = new();

    [ObservableProperty]
    Vec4 scale = new();

    [YamlMember(Alias = "components")]
    public List<int> ComponentIndices { get; set; } = [];

    [YamlIgnore]
    protected bool IsDirty
    {
        get => isDirty;
        set => SetProperty(ref isDirty, value);
    }

    [YamlIgnore]
    protected bool isDirty = false;

    [YamlIgnore]
    string? _lastCommittedYaml;
}
