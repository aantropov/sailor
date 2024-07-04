using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using System.Runtime.CompilerServices;
using SailorEditor.Services;

namespace SailorEditor.ViewModels;

public partial class GameObject : ObservableObject, ICloneable
{
    public string DisplayName { get { return Name; } }

    public int PrefabIndex = -1;

    public void MarkDirty([CallerMemberName] string propertyName = null) { IsDirty = true; OnPropertyChanged(propertyName); }

    public List<Component> Components { get { return MauiProgram.GetService<WorldService>().GetComponents(this); } }

    [YamlMember(Alias = "components")]
    public List<int> ComponentIndices { get; set; } = [];

    public object Clone() => new GameObject()
    {
        Name = Name + "(Clone)",
        Position = new Vec4(Position),
        Rotation = new Vec4(Rotation),
        Scale = new Vec4(Scale),
        ParentIndex = ParentIndex,
        InstanceId = InstanceId,
        ComponentIndices = new List<int>(ComponentIndices)
    };

    [ObservableProperty]
    string name = string.Empty;

    [ObservableProperty]
    Vec4 position = new();

    [ObservableProperty]
    Vec4 rotation = new();

    [ObservableProperty]
    Vec4 scale = new();

    [ObservableProperty]
    uint parentIndex = uint.MaxValue;

    [ObservableProperty]
    string instanceId = "NullInstanceId";

    [ObservableProperty]
    protected bool isDirty = false;

}
