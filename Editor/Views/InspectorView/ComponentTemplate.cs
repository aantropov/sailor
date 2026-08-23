using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Shell;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Views;
using SailorEngine;

namespace SailorEditor;
public partial class ComponentTemplate : DataTemplate
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
            IDispatcherTimer? animatorRuntimeTimer = null;
            props.Loaded += (_, _) => animatorRuntimeTimer?.Start();
            props.Unloaded += (_, _) => animatorRuntimeTimer?.Stop();

            props.BindingContextChanged += (sender, args) =>
            {
                animatorRuntimeTimer?.Stop();
                animatorRuntimeTimer = null;
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

                var dragGesture = new DragGestureRecognizer();
                dragGesture.DragStarting += (dragSender, dragArgs) =>
                {
                    dragArgs.Data.Properties[EditorDragDrop.DragItemKey] = component;
                };
                nameLabel.GestureRecognizers.Add(dragGesture);

                var worldService = MauiProgram.GetService<WorldService>();
                var clipboardService = MauiProgram.GetService<ComponentClipboardService>();
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
                        Text = "Copy Values",
                        Command = CreateContextMenuCommand(
                            () => clipboardService.CopyValuesAsync(component),
                            "Copy component values")
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Paste Values",
                        Command = CreateContextMenuCommand(
                            () => clipboardService.PasteValuesAsync(component),
                            "Paste component values")
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Reset to Defaults",
                        Command = CreateContextMenuCommand(
                            () => worldService.ResetComponentToDefaultsAsync(component),
                            "Reset component")
                    },
                    new EditorContextMenuItem
                    {
                        Text = "Remove Component",
                        Command = CreateContextMenuCommand(
                            () => worldService.RemoveComponentAsync(component),
                            "Remove component")
                    }
                };

                var flyout = contextMenuService.CreateFlyout(contextItems);
                FlyoutBase.SetContextFlyout(props, flyout);
                FlyoutBase.SetContextFlyout(
                    header,
                    contextMenuService.CreateFlyout(contextItems));
                FlyoutBase.SetContextFlyout(
                    nameLabel,
                    contextMenuService.CreateFlyout(contextItems));
                FlyoutBase.SetContextFlyout(
                    compactButton,
                    contextMenuService.CreateFlyout(contextItems));

                header.Add(compactButton, 0, 0);
                header.Add(nameLabel, 1, 0);
                Templates.AddGridRow(props, header, GridLength.Auto);
                Grid.SetColumnSpan(header, props.ColumnDefinitions.Count);

                if (isCompact)
                {
                    return;
                }

                if (component.Typename.Name == "Sailor::LandscapeComponent")
                {
                    AddLandscapeTools(props, component);
                    AddLandscapeImportEditor(props, component);
                }

                var engineTypes = MauiProgram.GetService<EngineService>().EngineTypes;

                foreach (var property in EnumerateInspectorProperties(component))
                {
                    // Internal info
                    if (property.Key == "fileId" || property.Key == "instanceId")
                        continue;

                    View propertyEditor = null;
                    var propertyDescriptor = component.Typename.Properties[property.Key];

                    if (propertyDescriptor is EnumProperty enumProp)
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
                        if (propertyDescriptor is ObjectPtrProperty objectPtr)
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
                                Observable<float> observableFloat when propertyDescriptor.Range is { } range => Templates.RangedFloatEditor(
                                    (Component vm) => observableFloat.Value,
                                    (vm, value) => observableFloat.Value = value,
                                    range),
                                Observable<int> observableInt when propertyDescriptor.Range is { } range => Templates.RangedIntEditor(
                                    (Component vm) => observableInt.Value,
                                    (vm, value) => observableInt.Value = value,
                                    range),
                                Observable<uint> observableUInt when propertyDescriptor.Range is { } range => Templates.RangedUIntEditor(
                                    (Component vm) => observableUInt.Value,
                                    (vm, value) => observableUInt.Value = value,
                                    range),
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
                                ObservableFileIdList fileIds => Templates.FileIdListEditor(
                                    fileIds,
                                    ResolveFileIdListSupportedType(
                                        component,
                                        property.Key)),
                                ObservableFloatList floatValues => Templates.FloatListEditor(floatValues),
                                Observable<InstanceId> observableInstanceId => Templates.InstanceIdEditor(component.OverrideProperties[property.Key], nameof(Observable<InstanceId>.Value), (Observable<InstanceId> vm) => vm.Value, (vm, value) => vm.Value = value),
                                _ => new Label { Text = "Unsupported property type" }
                            };
                    }

                    Templates.AddGridRowWithLabel(props, property.Key, propertyEditor, GridLength.Auto);
                }

                if (component.Typename.Name == "Sailor::LandscapeComponent")
                {
                    AddLandscapeVegetationEditor(props, component);
                }

                if (component.Typename.Name == "Sailor::AnimatorComponent")
                {
                    animatorRuntimeTimer = AddAnimatorRuntimeControls(props, component);
                    if (props.IsLoaded)
                    {
                        animatorRuntimeTimer.Start();
                    }
                }
            };

            return props;
        };
    }

    static IDispatcherTimer AddAnimatorRuntimeControls(Grid props, Component component)
    {
        var heading = new Label
        {
            Text = "Runtime Controller",
            FontAttributes = FontAttributes.Bold,
            Margin = new Thickness(0, 8, 0, 0)
        };
        Templates.AddGridRow(props, heading, GridLength.Auto);
        Grid.SetColumnSpan(heading, props.ColumnDefinitions.Count);

        var stateLabel = new Label
        {
            Text = "No controller",
            TextColor = Color.FromArgb("#929AA5"),
            FontSize = 11
        };
        Templates.AddGridRow(props, stateLabel, GridLength.Auto);
        Grid.SetColumnSpan(stateLabel, props.ColumnDefinitions.Count);

        var controllerFile = AnimatorRuntimeControls.ResolveController(component);
        if (controllerFile is not null)
        {
            foreach (var parameter in controllerFile.Parameters)
            {
                var editor = AnimatorRuntimeControls.CreateParameterEditor(
                    component,
                    parameter);
                Templates.AddGridRowWithLabel(
                    props,
                    parameter.Name,
                    editor,
                    GridLength.Auto);
            }
        }

        var timer = props.Dispatcher.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(100);
        var stateRequestPending = false;
        timer.Tick += async (_, _) =>
        {
            if (stateRequestPending || component.InstanceId is null || component.InstanceId.IsEmpty())
            {
                return;
            }
            stateRequestPending = true;
            try
            {
                var state = await MauiProgram.GetService<EngineService>()
                    .GetAnimatorStateAsync(component.InstanceId);
                if (state is null || !state.Value.HasController)
                {
                    stateLabel.Text = "No controller";
                    return;
                }
                stateLabel.Text = state.Value.IsTransitioning
                    ? $"{state.Value.ActiveStateName} → {state.Value.DestinationStateName}  {state.Value.TransitionAlpha:P0}"
                    : $"{state.Value.ActiveStateName}  {state.Value.ActiveStateTime:F2}s";
            }
            catch (Exception exception)
            {
                stateLabel.Text = exception.Message;
            }
            finally
            {
                stateRequestPending = false;
            }
        };
        return timer;
    }

    static string FormatComponentTypeName(string typeName)
    {
        const string prefix = "Sailor::";
        return !string.IsNullOrWhiteSpace(typeName) && typeName.StartsWith(prefix, StringComparison.Ordinal)
            ? typeName[prefix.Length..]
            : typeName;
    }

    static IEnumerable<KeyValuePair<string, ObservableObject>> EnumerateInspectorProperties(
        Component component)
    {
        if (component.Typename.Name == "Sailor::LandscapeComponent")
        {
            foreach (var property in component.OverrideProperties)
            {
                if (property.Key is "sculptStamps" or "paintStamps" or
                    "layerTextures" or "heightmapTexture" or "materialMasks" or
                    "vegetationModels" or "vegetationMaterials" or
                    "vegetationMeshIndex" or
                    "vegetationInstancesPerChunk" or "vegetationResidency" or
                    "vegetationPriority" or "vegetationMinScale" or
                    "vegetationMaxScale" or "vegetationGroundOffset" or
                    "vegetationShadowMode" or "vegetationShadowDistance" or
                    "vegetationMinLod" or "vegetationMaxLod" or
                    "vegetationLod1ScreenCoverage" or "vegetationLod2ScreenCoverage" or
                    "vegetationCullDistance" or "vegetationColliderRadius" or
                    "vegetationColliderHeight" or "vegetationColliderOffsetY" or
                    "regenerate" or "flatten")
                {
                    continue;
                }
                yield return property;
            }
            yield break;
        }

        if (component.Typename.Name != "Sailor::MeshRendererComponent")
        {
            foreach (var property in component.OverrideProperties)
            {
                yield return property;
            }

            yield break;
        }

        if (component.OverrideProperties.TryGetValue("model", out var model))
        {
            yield return new KeyValuePair<string, ObservableObject>("model", model);
        }

        if (component.OverrideProperties.TryGetValue("overrideMaterials", out var overrideMaterials))
        {
            yield return new KeyValuePair<string, ObservableObject>("overrideMaterials", overrideMaterials);
        }

        foreach (var property in component.OverrideProperties)
        {
            if (property.Key is "model" or "overrideMaterials")
            {
                continue;
            }

            yield return property;
        }
    }

    static Type ResolveFileIdListSupportedType(
        Component component,
        string propertyName)
    {
        return component.Typename.Name == "Sailor::MeshRendererComponent" &&
            propertyName == "overrideMaterials"
            ? typeof(MaterialFile)
            : component.Typename.Name == "Sailor::LandscapeComponent" &&
                propertyName == "layerTextures"
                ? typeof(TextureFile)
                : null;
    }

    static Command CreateContextMenuCommand(
        Func<Task> action,
        string operation)
    {
        return new Command(
            () => RunContextMenuAction(
                action,
                operation));
    }

    static void RunContextMenuAction(
        Func<Task> action,
        string operation)
    {
        _ = ExecuteContextMenuActionAsync(
            action,
            operation);
    }

    static async Task ExecuteContextMenuActionAsync(
        Func<Task> action,
        string operation)
    {
        try
        {
            await action();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"{operation} failed: {ex}");
            try
            {
                MauiProgram.GetService<EditorShellHost>()
                    .SetStatus($"{operation} failed: {ex.Message}");
            }
            catch (Exception statusException)
            {
                Console.Error.WriteLine(
                    $"Failed to publish component action error status: {statusException}");
            }
        }
    }
}
