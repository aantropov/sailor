using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Views;
using SailorEngine;

namespace SailorEditor;
public class ComponentTemplate : DataTemplate
{
    static readonly HashSet<string> CompactComponents = [];

    public ComponentTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                    new ColumnDefinition { Width = new GridLength(2, GridUnitType.Star) }
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

                var compactButton = new Button
                {
                    Text = "-",
                    WidthRequest = 24,
                    HeightRequest = 24,
                    Padding = new Thickness(0),
                    Margin = new Thickness(0),
                    BackgroundColor = Colors.Transparent,
                    BorderWidth = 0,
                    MinimumHeightRequest = 24,
                    MinimumWidthRequest = 24
                };

                var nameLabel = new Label { Text = "DisplayName", VerticalOptions = LayoutOptions.Center, HorizontalTextAlignment = TextAlignment.Start, FontAttributes = FontAttributes.Bold };
                nameLabel.Behaviors.Add(new DisplayNameBehavior());
                nameLabel.Text = FormatComponentTypeName(component.Typename.Name);

                var worldService = MauiProgram.GetService<WorldService>();
                var contextMenuService = MauiProgram.GetService<EditorContextMenuService>();
                var componentKey = component.InstanceId?.ToString() ?? component.GetHashCode().ToString();
                var isCompact = CompactComponents.Contains(componentKey);

                compactButton.Text = isCompact ? "+" : "-";
                compactButton.Clicked += (buttonSender, clickArgs) =>
                {
                    if (CompactComponents.Contains(componentKey))
                    {
                        CompactComponents.Remove(componentKey);
                    }
                    else
                    {
                        CompactComponents.Add(componentKey);
                    }

                    if (props.BindingContext is Component reboundComponent)
                    {
                        props.BindingContext = null;
                        props.BindingContext = reboundComponent;
                    }
                };

                var contextItems = new[]
                {
                    new EditorContextMenuItem
                    {
                        Text = "Reset to Defaults",
                        Command = new Command(() => worldService.ResetComponentToDefaults(component))
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Remove Component",
                        Command = new Command(() => worldService.RemoveComponent(component))
                    }
                };

                var flyout = contextMenuService.CreateFlyout(contextItems);
                FlyoutBase.SetContextFlyout(props, flyout);
                FlyoutBase.SetContextFlyout(header, flyout);
                FlyoutBase.SetContextFlyout(nameLabel, flyout);
                FlyoutBase.SetContextFlyout(compactButton, flyout);

                header.Add(compactButton, 0, 0);
                header.Add(nameLabel, 1, 0);
                Templates.AddGridRow(props, header, GridLength.Auto);
                Grid.SetColumnSpan(header, props.ColumnDefinitions.Count);

                if (isCompact)
                {
                    return;
                }

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
                        if (engineTypes.Enums.TryGetValue(enumProp.Typename, out var enumValues))
                        {
                            propertyEditor = Templates.EnumPicker(enumValues,
                                (Component vm) => observableString.Value, (vm, value) => observableString.Value = value);
                        }
                        else
                        {
                            propertyEditor = new Label
                            {
                                Text = $"Missing enum metadata: {enumProp.Typename}",
                                VerticalTextAlignment = TextAlignment.Center
                            };
                        }
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
                                Observable<int> observableInt => Templates.IntEditor((Component vm) => observableInt.Value, (vm, value) => observableInt.Value = value),
                                Observable<uint> observableUInt => Templates.UIntEditor((Component vm) => observableUInt.Value, (vm, value) => observableUInt.Value = value),
                                Observable<bool> observableBool => Templates.BoolEditor((Component vm) => observableBool.Value, (vm, value) => observableBool.Value = value),
                                Observable<string> observableString => Templates.StringEditor((Component vm) => observableString.Value, (vm, value) => observableString.Value = value),
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
