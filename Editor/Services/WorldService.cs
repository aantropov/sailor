using SailorEditor.ViewModels;

namespace SailorEditor.Services
{
    public class WorldService
    {
        public World Current { get; private set; } = new World();
        public List<GameObject> GameObjects { get; private set; } = new List<GameObject>();
        public List<Component> Components { get; private set; } = new List<Component>();
    }
}
