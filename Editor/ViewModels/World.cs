using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using YamlDotNet.Serialization;

namespace SailorEditor.ViewModels
{
    public partial class World : ObservableObject, ICloneable
    {
        public object Clone() => new World() { Name = Name + "(Clone)", Prefabs = new ObservableList<Prefab>(Prefabs) };

        [ObservableProperty]
        string name = string.Empty;

        [ObservableProperty]
        ObservableList<Prefab> prefabs = new();

        [ObservableProperty]
        protected bool isDirty = false;
    }
}
