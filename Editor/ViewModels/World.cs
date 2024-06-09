using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;

namespace SailorEditor.ViewModels
{
    public partial class World : ObservableObject//, ICloneable
    {
        public int Id { get; set; } = 1;
        public string Name { get; set; }

        [ObservableProperty]
        ObservableList<GameObject> gameObjects = new();

        [ObservableProperty]
        ObservableList<Component> components = new();

        //public object Clone() => new World() { Id = Id, Name = Name, GameObjects = new List<GameObject>(GameObjects), Components = new List<Component>(Components);};

        [ObservableProperty]
        protected bool isDirty = false;
    }
}
