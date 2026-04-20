using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Views;
using SailorEngine;
using YamlDotNet.Core.Tokens;

namespace SailorEditor;
public class ComponentTemplate : DataTemplate
{
    public ComponentTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = new GridLength(Templates.InspectorLabelColumnWidth) },
                    new ColumnDefinition { Width = GridLength.Star }
                },
                ColumnSpacing = Templates.InspectorFieldSpacing,
                RowSpacing = 4,
                BackgroundColor = Colors.Transparent,
                HorizontalOptions = LayoutOptions.Fill,
                MinimumWidthRequest = 0
            };

            props.BindingContextChanged += (sender, args) =>
            {
                if (((Grid)sender).BindingContext is not Component component)
                {
                    props.Children.Clear();
                    props.RowDefinitions.Clear();
                    props.GestureRecognizers.Clear();
                    return;
                }

                props.Children.Clear();
                props.RowDefinitions.Clear();

                props.GestureRecognizers.Clear();
                var dragGesture = new DragGestureRecognizer();
                dragGesture.DragStarting += (dragSender, dragArgs) =>
                {
                    dragArgs.Data.Properties[EditorDragDrop.DragItemKey] = component;
                };
                props.GestureRecognizers.Add(dragGesture);

                var header = new Grid
                {
                    HorizontalOptions = LayoutOptions.Fill,
                    MinimumWidthRequest = 0,
                    ColumnDefinitions =
                    {
                        new ColumnDefinition { Width = GridLength.Auto },
                        new ColumnDefinition { Width = GridLength.Star }
                    },
                    ColumnSpacing = Templates.InspectorFieldSpacing
                };

                var removeButton = new Button { Text = "-", WidthRequest = 32, HeightRequest = 32 };
                removeButton.Clicked += (buttonSender, clickArgs) => MauiProgram.GetService<WorldService>().RemoveComponent(component);

                var nameLabel = new Label { Text = "DisplayName", VerticalOptions = LayoutOptions.Center, HorizontalTextAlignment = TextAlignment.Start, FontAttributes = FontAttributes.Bold };
                nameLabel.Behaviors.Add(new DisplayNameBehavior());
                nameLabel.Text = FormatComponentTypeName(component.Typename.Name);

                var resetFlyout = MauiProgram.GetService<EditorContextMenuService>().CreateFlyout(new EditorContextMenuItem
                    {
                        Text = "Reset to Defaults",
                        Command = new Command(() => MauiProgram.GetService<WorldService>().ResetComponentToDefaults(component))
                    });
                FlyoutBase.SetContextFlyout(props, resetFlyout);
                FlyoutBase.SetContextFlyout(nameLabel, resetFlyout);

                var contextGesture = new TapGestureRecognizer { Buttons = ButtonsMask.Secondary };
                contextGesture.Tapped += (gestureSender, gestureArgs) =>
                {
                    MauiProgram.GetService<EditorContextMenuService>().Show(new EditorContextMenuItem
                    {
                        Text = "Reset to Defaults",
                        Command = new Command(() => MauiProgram.GetService<WorldService>().ResetComponentToDefaults(component))
                    });
                };
                props.GestureRecognizers.Add(contextGesture);

                header.Add(removeButton, 0, 0);
                header.Add(nameLabel, 1, 0);
                Templates.AddGridRow(props, header, GridLength.Auto);
                Grid.SetColumnSpan(header, props.ColumnDefinitions.Count);

                var engineTypes = MauiProgram.GetService<EngineService>().EngineTypes;

                foreach (var property in component.OverrideProperties)
                {
                    // Internal info
                    if (property.Key == "fileId" || property.Key == "instanceId")
                        continue;

                    View propertyEditor = null;

                    if (component.Typename.Properties[property.Key] is EnumProperty enumProp)
                    {
                        var observableString = property.Value as Observable<string>;
                        propertyEditor = Templates.EnumPicker(engineTypes.Enums[enumProp.Typename],
                            (Component vm) => observableString.Value, (vm, value) => observableString.Value = value);
                    }
                    else
                    {
                        if (component.Typename.Properties[property.Key] is ObjectPtrProperty objectPtr)
                        {
                            if (property.Value is ObjectPtr ptr)
                            {
                                if (!ptr.FileId.IsEmpty())
                                {
                                    propertyEditor = Templates.FileIdEditor(ptr,
                                        nameof(ObjectPtr.FileId), (ObjectPtr p) => p.FileId, (p, value) => p.FileId = value, objectPtr.GenericType);
                                }
                                else if (!ptr.InstanceId.IsEmpty() || objectPtr.CouldBeInstantiated)
                                {
                                    propertyEditor = Templates.InstanceIdEditor(ptr,
                                        nameof(ObjectPtr.InstanceId), (ObjectPtr vm) => ptr.InstanceId, (p, value) => p.InstanceId = value, objectPtr.GenericTypename);
                                }
                                else
                                {
                                    propertyEditor = Templates.FileIdEditor(ptr,
                                       nameof(ObjectPtr.FileId), (ObjectPtr p) => p.FileId, (p, value) => p.FileId = value, objectPtr.GenericType);
                                }
                            }
                        }
                        else
                            propertyEditor = property.Value switch
                            {
                                Observable<float> observableFloat => Templates.FloatEditor((Component vm) => observableFloat.Value, (vm, value) => observableFloat.Value = value),
                                Rotation quat => Templates.RotationEditor((Component vm) => quat),
                                Vec4 vec4 => Templates.Vec4Editor((Component vm) => vec4),
                                Vec3 vec3 => Templates.Vec3Editor((Component vm) => vec3),
                                Vec2 vec2 => Templates.Vec2Editor((Component vm) => vec2),
                                Observable<FileId> observableFileId => Templates.FileIdEditor(component.OverrideProperties[property.Key], nameof(Observable<FileId>.Value), (Observable<FileId> vm) => vm.Value, (vm, value) => vm.Value = value),
                                Observable<InstanceId> observableInstanceId => Templates.InstanceIdEditor(component.OverrideProperties[property.Key], nameof(Observable<InstanceId>.Value), (Observable<InstanceId> vm) => vm.Value, (vm, value) => vm.Value = value),
                                _ => new Label { Text = "Unsupported property type" }
                            };
                    }

                    Templates.AddGridRowWithLabel(props, property.Key, propertyEditor, GridLength.Auto);
                }
            };

            return props;
        };
    }

    static string FormatComponentTypeName(string typeName)
    {
        const string prefix = "Sailor::";
        return !string.IsNullOrWhiteSpace(typeName) && typeName.StartsWith(prefix, StringComparison.Ordinal)
            ? typeName[prefix.Length..]
            : typeName;
    }
}
