using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using YamlDotNet.Serialization;

namespace SailorEditor.ViewModels
{
    public partial class World : ObservableObject, ICloneable
    {
        public object Clone() => new World() { Name = Name/*, Prefabs = new ObservableList<Prefab>(Prefabs) */};

        [ObservableProperty]
        [YamlMember(Alias = "name")]
        public string name = string.Empty;

        [ObservableProperty]
        [YamlMember(Alias = "prefabs")]
        public ObservableList<Prefab> prefabs = new();

        [ObservableProperty]
        protected bool isDirty = false;
    }
}
