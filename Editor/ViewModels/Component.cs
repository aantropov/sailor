using CommunityToolkit.Mvvm.ComponentModel;

namespace SailorEditor.ViewModels
{
    public partial class Component : ObservableObject, ICloneable
    {
        public Component()
        {
            PropertyChanged += (s, args) =>
            {
                if (args.PropertyName != "IsDirty")
                {
                    IsDirty = true;
                }
            };
        }

        public int Id { get; set; }
        public int GameObjectId { get; set; }
        
        public object Clone() => new Component();

        [ObservableProperty]
        protected bool isDirty = false;

        [ObservableProperty]
        protected string displayName;
    }
}