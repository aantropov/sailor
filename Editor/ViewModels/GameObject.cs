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
using System.Windows.Input;
using SailorEngine;

namespace SailorEditor.ViewModels;

public partial class GameObject : ObservableObject, ICloneable
{
    public GameObject()
    {
        AddNewComponent = new Command(async () => await OnAddNewComponent());
        ClearComponentsCommand = new Command(OnClearComponents);

        PropertyChanged += (s, args) =>
        {
            if (args.PropertyName != nameof(IsDirty))
                IsDirty = true;

            CommitChanges();
        };
    }

    public void CommitChanges()
    {
        if (!IsDirty || !isInited)
            return;

        var yamlGameObject = EditorYaml.SerializeGameObject(this);
        var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
        var contextProvider = MauiProgram.GetService<IActionContextProvider>();
        var result = dispatcher.DispatchAsync(
            new UpdateGameObjectCommand(this, _lastCommittedYaml ?? yamlGameObject, yamlGameObject, $"Edit {Name}"),
            contextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.UI, nameof(CommitChanges))))
            .GetAwaiter()
            .GetResult();

        if (result.Succeeded)
            _lastCommittedYaml = yamlGameObject;

        IsDirty = false;
    }

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

        //foreach (var component in Components)
        //    component.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Components));

        IsDirty = false;
        _lastCommittedYaml = EditorYaml.SerializeGameObject(this);
        isInited = true;
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

    async Task OnAddNewComponent()
    {
        var componentTypeName = await Views.AddComponentDialogPage.ShowAsync(
            MauiProgram.GetService<EngineService>().EngineTypes.Components.Values
                .Where(IsComponentType)
                .Select(type => type.Name)
                .OrderBy(name => name)
                .ToList());

        if (!string.IsNullOrWhiteSpace(componentTypeName))
        {
            MauiProgram.GetService<WorldService>().AddComponent(this, componentTypeName);
        }
    }

    static bool IsComponentType(ComponentType type)
    {
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

    void OnClearComponents() { }

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
