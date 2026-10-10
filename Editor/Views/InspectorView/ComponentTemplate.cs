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
                nameLabel.Text = component.IsUndefined
                    ? $"{component.Typename.Name} (Undefined)"
                    : FormatComponentTypeName(component.Typename.Name);

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

                if (component.IsUndefined)
                {
                    contextItems = [contextItems[^1]];
                }

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

                if (component.IsUndefined)
                {
                    var values = new Label
                    {
                        Text = SerializationUtils.CreateSerializerBuilder().Build()
                            .Serialize(component.PreservedReadOnlyProperties).TrimEnd(),
                        LineBreakMode = LineBreakMode.WordWrap
                    };
                    Templates.AddGridRow(props, values, GridLength.Auto);
                    Grid.SetColumnSpan(values, props.ColumnDefinitions.Count);
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

                    var propertyEditor = CreateReflectedValueEditor(
                        component, engineTypes, component.Typename.Properties[property.Key],
                        property.Value, component.Typename.Name, property.Key);

                    AddReflectedEditorRow(
                        props,
                        InspectorPropertyPresentation.FormatPropertyName(
                            component.Typename.Name,
                            property.Key),
                        propertyEditor,
                        component.Typename.Properties[property.Key]);
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
                if (property.Key is "layerTextures" or "heightmapTexture" or "materialMasks" or
                    "sculptStamps" or "paintStamps" or "regenerate" or "flatten" or "saveVegetation")
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

    static Type ResolveFileIdSupportedType(
        Component component,
        string propertyName)
    {
        if (component.Typename.Name != "Sailor::LandscapeComponent")
        {
            return null;
        }
        return propertyName switch
        {
            "vegetation" => typeof(LandscapeVegetationFile),
            _ => null
        };
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
