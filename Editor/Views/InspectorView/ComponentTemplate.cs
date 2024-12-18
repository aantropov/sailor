﻿using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Views;
using SailorEngine;
using Windows.Foundation.Collections;
using YamlDotNet.Core.Tokens;

namespace SailorEditor;
public class ComponentTemplate : DataTemplate
{
    public ComponentTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto } } };

            props.BindingContextChanged += (sender, args) =>
            {
                var component = (Component)((Grid)sender).BindingContext;
                props.Children.Clear();

                var nameLabel = new Label { Text = "DisplayName", VerticalOptions = LayoutOptions.Center, HorizontalTextAlignment = TextAlignment.Start, FontAttributes = FontAttributes.Bold };
                nameLabel.Behaviors.Add(new DisplayNameBehavior());
                nameLabel.Text = component.Typename.Name;

                Templates.AddGridRow(props, nameLabel, GridLength.Auto);

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
                                        nameof(ObjectPtr.InstanceId), (ObjectPtr vm) => ptr.InstanceId, (p, value) => p.InstanceId = value);
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
                                _ => new Label { Text = "Unsupported property type" }
                            };
                    }

                    Templates.AddGridRowWithLabel(props, property.Key, propertyEditor, GridLength.Auto);
                }
            };

            return props;
        };
    }
}
