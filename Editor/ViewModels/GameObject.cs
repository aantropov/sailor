using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Xml.Linq;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEditor.ViewModels
{
    public partial class GameObject : ObservableObject, ICloneable
    {
        public List<int> Components = new();

        public object Clone() => new GameObject()
        {
            Name = Name + "(Clone)",
            Position = new Vec4(Position),
            Rotation = new Vec4(Rotation),
            Scale = new Vec4(Scale),
            ParentIndex = ParentIndex,
            InstanceId = InstanceId,
            Components = new List<int>(Components)
        };

        [ObservableProperty]
        string name = string.Empty;

        [ObservableProperty]
        Vec4 position = new Vec4();

        [ObservableProperty]
        Vec4 rotation = new Vec4();

        [ObservableProperty]
        Vec4 scale = new Vec4();

        [ObservableProperty]
        uint parentIndex = uint.MaxValue;

        [ObservableProperty]
        string instanceId = "NullInstanceId";
    }
}
