using SailorEditor.Helpers;
using SailorEditor.Utility;

namespace SailorEditor;

public sealed class AggregateInspectorTemplate : DataTemplate
{
    public AggregateInspectorTemplate()
    {
        LoadTemplate = () =>
        {
            var layout = new VerticalStackLayout { Spacing = 8 };
            layout.BindingContextChanged += (_, _) => Build(layout);
            return layout;
        };
    }

    static void Build(VerticalStackLayout layout)
    {
        layout.Children.Clear();
        if (layout.BindingContext is not AggregateInspectorModel model)
        {
            return;
        }
        if (model.IsGameObjectSelection)
        {
            var properties = model.Sections.Single(section => section.Name == "Object").Fields;
            layout.Children.Add(ComponentTemplate.CreateAggregateFieldsGrid(properties, Templates.InspectorLabelColumnWidth));
            AddTemplate(layout, new GameObjectTemplate(), new AggregateGameObjectInspector(model));
            return;
        }
        foreach (var section in model.Sections)
        {
            if (section.IsComponent)
            {
                AddTemplate(layout, new ComponentTemplate(), section);
            }
            else
            {
                layout.Children.Add(new Label { Text = section.Name, FontAttributes = FontAttributes.Bold });
                layout.Children.Add(ComponentTemplate.CreateAggregateFieldsGrid(section.Fields));
            }
        }
        if (model.Sections.Count == 0)
        {
            layout.Children.Add(new Label { Text = "No common editable properties.", TextColor = Colors.Gray });
        }
    }

    static void AddTemplate(VerticalStackLayout layout, DataTemplate template, object bindingContext)
    {
        if (template.CreateContent() is View view)
        {
            view.BindingContext = bindingContext;
            layout.Children.Add(view);
        }
    }
}
