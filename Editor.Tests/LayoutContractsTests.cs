using SailorEditor.Layout;
using SailorEditor.Panels;
using System.Xml.Linq;

namespace SailorEditor.Editor.Tests;

public class LayoutContractsTests
{
    [Fact]
    public void MaterialInspector_DynamicSectionsUseContentSizedLayouts()
    {
        var repositoryRoot = ResolveRepositoryRoot();
        var templatePath = Path.Combine(
            repositoryRoot,
            "Editor",
            "Views",
            "InspectorView",
            "MaterialFileTemplate.xaml");
        var document = XDocument.Load(templatePath);

        Assert.DoesNotContain(
            document.Descendants(),
            element => element.Name.LocalName == "CollectionView");

        var contentSizedBindings = document.Descendants().
            Where(element => element.Name.LocalName == "VerticalStackLayout").
            SelectMany(element => element.Attributes()).
            Where(attribute => attribute.Name.LocalName == "BindableLayout.ItemsSource").
            Select(attribute => attribute.Value).
            ToHashSet(StringComparer.Ordinal);

        Assert.Contains("{Binding Samplers}", contentSizedBindings);
        Assert.Contains("{Binding UniformsVec4}", contentSizedBindings);
        Assert.Contains("{Binding UniformsFloat}", contentSizedBindings);
        Assert.Contains("{Binding ShaderDefines}", contentSizedBindings);
    }

    [Fact]
    public void EditorLayout_MinimalGraph_PreservesExpectedStructure()
    {
        var panelId = PanelId.New();
        var layout = new EditorLayout(
            Version: 1,
            Root: new LayoutRoot(
                new TabGroupNode(
                    PanelRole.Tool,
                    [new PanelReference(panelId, new PanelTypeId("Hierarchy"))],
                    ActivePanelId: panelId,
                    GroupId: "left-tools")),
            MainWindow: new WindowBounds(10, 20, 1600, 900));

        Assert.Equal(1, layout.Version);
        var tabs = Assert.IsType<TabGroupNode>(layout.Root.Content);
        Assert.Equal(panelId, tabs.ActivePanelId);
        Assert.Single(tabs.Panels);
        Assert.Equal("Hierarchy", tabs.Panels[0].PanelTypeId.Value);
    }

    static string ResolveRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (File.Exists(Path.Combine(current.FullName, "CMakeLists.txt")) &&
                Directory.Exists(Path.Combine(current.FullName, "Editor")))
            {
                return current.FullName;
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
