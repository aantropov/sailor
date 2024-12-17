using CommunityToolkit.Mvvm.ComponentModel;
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
using Windows.ApplicationModel.VoiceCommands;

namespace SailorEditor.ViewModels;

public partial class GameObject : ObservableObject, ICloneable
{
    public GameObject()
    {
        AddNewComponent = new Command(OnAddNewComponent);
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

        string yamlComponent = string.Empty;

        using (var writer = new StringWriter())
        {
            var serializer = new SerializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .WithTypeConverter(new InstanceIdYamlConverter())
                .WithTypeConverter(new RotationYamlConverter())
                .WithTypeConverter(new Vec4YamlConverter())
                .Build();

            var yaml = serializer.Serialize(this);
            writer.Write(yaml);

            yamlComponent = writer.ToString();
        }

        MauiProgram.GetService<EngineService>().CommitChanges(InstanceId, yamlComponent);

        IsDirty = false;
    }

    public void Initialize()
    {
        Scale.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Scale));
        Position.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Position));
        Rotation.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Rotation));

        //foreach (var component in Components)
        //    component.PropertyChanged += (a, e) => OnPropertyChanged(nameof(Components));

        IsDirty = false;
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

    void OnAddNewComponent() { }
    void OnClearComponents() { }

    [ObservableProperty]
    string name = string.Empty;

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
}
