using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;
using System.ComponentModel;

namespace SailorEditor.ViewModels;

public partial class Prefab : ObservableObject, ICloneable
{
    public object Clone() => new Prefab()
    {
        GameObjects = new ObservableList<GameObject>(GameObjects),
        Components = new ObservableList<Component>(Components)
    };

    [ObservableProperty]
    ObservableList<GameObject> gameObjects = [];

    [ObservableProperty]
    ObservableList<Component> components = [];
}
