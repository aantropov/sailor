using SailorEditor.Views;

namespace SailorEditor.Panels;

public static class KnownPanelTypes
{
    public static readonly PanelTypeId Scene = new("Scene");
    public static readonly PanelTypeId Hierarchy = new("Hierarchy");
    public static readonly PanelTypeId Inspector = new("Inspector");
    public static readonly PanelTypeId Content = new("Content");
    public static readonly PanelTypeId Console = new("Console");
    public static readonly PanelTypeId Settings = new("Settings");
    public static readonly PanelTypeId AI = new("AI");
}

public sealed class PanelRegistry : IPanelDescriptorRegistry
{
    readonly Dictionary<PanelTypeId, PanelDescriptor> _descriptors = new();

    public PanelRegistry()
    {
        Register(new PanelDescriptor(KnownPanelTypes.Scene, "Scene", PanelRole.Document, false, new(DefaultPosition: DockPosition.Center), () => new SceneView()));
        Register(new PanelDescriptor(KnownPanelTypes.Hierarchy, "Hierarchy", PanelRole.Tool, false, new(DefaultPosition: DockPosition.Left), () => new HierarchyView()));
        Register(new PanelDescriptor(KnownPanelTypes.Inspector, "Inspector", PanelRole.Tool, false, new(DefaultPosition: DockPosition.Right), () => new InspectorView()));
        Register(new PanelDescriptor(KnownPanelTypes.Content, "Content", PanelRole.Tool, false, new(DefaultPosition: DockPosition.Left), () => new ContentFolderView()));
        Register(new PanelDescriptor(KnownPanelTypes.Console, "Console", PanelRole.Tool, false, new(DefaultPosition: DockPosition.Bottom), () => new ConsoleView()));
        Register(new PanelDescriptor(KnownPanelTypes.Settings, "Settings", PanelRole.Tool, false, new(DefaultPosition: DockPosition.Right), () => new PlaceholderPanelView("Settings", "Unified settings shell surface placeholder.")));
        Register(new PanelDescriptor(KnownPanelTypes.AI, "AI", PanelRole.Tool, false, new(DefaultPosition: DockPosition.Right), () => new PlaceholderPanelView("AI", "Native AI panel placeholder.")));
    }

    public void Register(PanelDescriptor descriptor) => _descriptors[descriptor.TypeId] = descriptor;

    public bool TryGetDescriptor(PanelTypeId typeId, out PanelDescriptor descriptor) => _descriptors.TryGetValue(typeId, out descriptor!);

    public IReadOnlyCollection<PanelDescriptor> GetAllDescriptors() => _descriptors.Values.ToArray();
}
