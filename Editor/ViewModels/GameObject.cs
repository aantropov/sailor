using CommunityToolkit.Mvvm.ComponentModel;
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
    bool CommitInspectorChanges();
}

public partial class GameObject : ObservableObject, ICloneable, IInspectorEditable
{
    readonly InspectorAutoCommitController _autoCommit = new(
        propertyName => propertyName == nameof(IsDirty),
        propertyName => false);

    public GameObject()
    {
        AddNewComponent = new Command(async () => await AddComponentFromInspectorAsync());
        ClearComponentsCommand = new Command(ClearComponentsFromInspector);

        PropertyChanged += (s, args) =>
        {
            var decision = _autoCommit.OnPropertyChanged(args.PropertyName);
            if (decision.MarkDirty)
                IsDirty = true;
        };
    }

    public bool CommitInspectorChanges()
    {
        if (!_autoCommit.ShouldCommitPendingChanges(IsDirty) || !isInited)
            return false;

        var yamlGameObject = EditorYaml.SerializeGameObject(this);
        var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
        var contextProvider = MauiProgram.GetService<IActionContextProvider>();
        var result = dispatcher.DispatchAsync(
            new UpdateGameObjectCommand(this, _lastCommittedYaml ?? yamlGameObject, yamlGameObject, $"Edit {Name}"),
            contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(CommitInspectorChanges))))
            .GetAwaiter()
            .GetResult();

        if (result.Succeeded)
        {
            _lastCommittedYaml = yamlGameObject;
            IsDirty = false;
            return true;
        }

        return false;
    }

    [YamlIgnore]
    public bool HasPendingInspectorChanges => IsDirty;

    public void Initialize()
    {
        InstanceId ??= InstanceId.NullInstanceId;
        Position ??= new Vec4();
        Rotation ??= new Rotation();
        Scale ??= new Vec4();
        ComponentIndices ??= [];

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
    public int PrefabIndex = -1;

    public void MarkDirty([CallerMemberName] string propertyName = null) { IsDirty = true; OnPropertyChanged(propertyName); }

    [YamlIgnore]
    public List<Component> Components { get { return MauiProgram.GetService<WorldService>().GetComponents(this); } }

    public object Clone() => new GameObject()
    {
        Name = Name + "(Clone)",
        Position = new Vec4(Position),
        Rotation = new Rotation(Rotation),
        Scale = new Vec4(Scale),
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
        var componentTypeName = await Views.AddComponentDialogPage.ShowAsync(
            MauiProgram.GetService<EngineService>().EngineTypes.Components.Values
                .Where(IsComponentType)
                .Select(type => type.Name)
                .OrderBy(name => name)
                .ToList());

        if (!string.IsNullOrWhiteSpace(componentTypeName))
        {
            await Task.Run(() => MauiProgram.GetService<WorldService>().AddComponent(this, componentTypeName));
        }
    }

    static bool IsComponentType(ComponentType type)
    {
        if (type.Name == "Sailor::Component")
        {
            return false;
        }

        var engineTypes = MauiProgram.GetService<EngineService>().EngineTypes;
        var baseType = type.Base;

        while (!string.IsNullOrEmpty(baseType))
        {
            if (baseType == "Sailor::Component")
            {
                return true;
            }

            baseType = engineTypes.Components[baseType].Base;
        }

        return false;
    }

    public async void ClearComponentsFromInspector()
    {
        var components = Components.ToList();
        await Task.Run(() =>
        {
            foreach (var component in components)
            {
                MauiProgram.GetService<WorldService>().RemoveComponent(component);
            }
        });
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
