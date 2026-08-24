namespace SailorEditor;

public sealed class WorldFileTemplate : DataTemplate
{
    public WorldFileTemplate()
    {
        LoadTemplate = static () => new Views.ControlPanelView();
    }
}
