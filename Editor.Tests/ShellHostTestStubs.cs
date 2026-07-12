namespace Microsoft.Maui.Controls
{
    public interface IValueConverter
    {
        object Convert(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture);
        object ConvertBack(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture);
    }

    public class BindableProperty
    {
        public static readonly object UnsetValue = new();
    }

    public class View;

    public class Label : View
    {
        public string? Text { get; set; }
    }

    public class ContentView : View;
}

namespace SailorEditor.Views
{
    public sealed class SceneView : Microsoft.Maui.Controls.ContentView;
    public sealed class HierarchyView : Microsoft.Maui.Controls.ContentView;
    public sealed class InspectorView : Microsoft.Maui.Controls.ContentView;
    public sealed class ContentFolderView : Microsoft.Maui.Controls.ContentView;
    public sealed class ConsoleView : Microsoft.Maui.Controls.ContentView;
    public sealed class SettingsPanelView : Microsoft.Maui.Controls.ContentView;
    public sealed class AIPanelView : Microsoft.Maui.Controls.ContentView;
    public sealed class EditorPerformanceView : Microsoft.Maui.Controls.ContentView;
}
