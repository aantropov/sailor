using CommunityToolkit.Mvvm.ComponentModel;

namespace SailorEditor.ViewModels
{
    public class GameObject : ObservableObject, ICloneable
    {
        public int Id { get; set; }
        public string Name { get; set; }
        public int ParentGameObjectId { get; set; }
        public int WorldId { get; set; }

        public object Clone() => new GameObject() { Id = Id, Name = Name + "(Clone)", ParentGameObjectId = ParentGameObjectId, WorldId = WorldId };
    }
}
