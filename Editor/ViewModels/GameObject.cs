namespace SailorEditor.ViewModels
{
    public class GameObject
    {
        public int Id { get; set; }
        public string Name { get; set; }
        public int ParentGameObjectId { get; set; }
        public int WorldId { get; set; }
    }
}
