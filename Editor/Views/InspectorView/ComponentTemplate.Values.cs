using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Helpers;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Collections.Specialized;

namespace SailorEditor;

public partial class ComponentTemplate
{
    static View CreateReflectedValueEditor(
        Component component, EngineTypes engineTypes, PropertyBase propertyDescriptor,
        ObservableObject value, string ownerType, string propertyName)
    {
        View propertyEditor = null;

        if (propertyDescriptor is RecordProperty recordType && value is ObservableRecord record)
            return CreateRecordEditor(component, engineTypes, recordType, record);
        if (propertyDescriptor is ListProperty listType && value is ObservableValueList list)
            return CreateValueListEditor(component, engineTypes, listType, list, ownerType, propertyName);

        if (propertyDescriptor is EnumProperty enumProp)
        {
            var observableString = value as Observable<string>;
            if (engineTypes.Enums.TryGetValue(enumProp.Typename, out var enumValues))
            {
                propertyEditor = Templates.EnumPicker(enumValues,
                    (Component vm) => observableString.Value,
                    (vm, value) => observableString.Value = value,
                    value => InspectorPropertyPresentation.FormatEnumValue(
                        ownerType,
                        propertyName,
                        value));
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
                if (value is ObjectPtr ptr)
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
                propertyEditor = value switch
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
                    Observable<FileId> observableFileId => Templates.FileIdEditor(
                        value,
                        nameof(Observable<FileId>.Value),
                        (Observable<FileId> vm) => vm.Value,
                        (vm, value) => vm.Value = value,
                        ownerType == "Sailor::LandscapeVegetationSettings"
                            ? propertyName == "modelFileId" ? typeof(ModelFile) : typeof(MaterialFile)
                            : ResolveFileIdSupportedType(component, propertyName)),
                    ObservableFileIdList fileIds => Templates.FileIdListEditor(
                        fileIds,
                        ResolveFileIdListSupportedType(
                            component,
                            propertyName)),
                    ObservableFloatList floatValues => Templates.FloatListEditor(floatValues),
                    Observable<InstanceId> observableInstanceId => Templates.InstanceIdEditor(value, nameof(Observable<InstanceId>.Value), (Observable<InstanceId> vm) => vm.Value, (vm, value) => vm.Value = value),
                    _ => new Label { Text = "Unsupported property type" }
                };
        }
        return propertyEditor;
    }

    static Grid CreateRecordEditor(
        Component component, EngineTypes engineTypes, RecordProperty descriptor, ObservableRecord record)
    {
        var fields = new Grid
        {
            ColumnDefinitions = { new ColumnDefinition(GridLength.Star), new ColumnDefinition(new GridLength(2, GridUnitType.Star)) },
            ColumnSpacing = Templates.InspectorFieldSpacing,
            RowSpacing = 4,
            MinimumWidthRequest = 0
        };
        foreach (var field in record.Fields)
        {
            var editor = CreateReflectedValueEditor(
                component, engineTypes, descriptor.RecordType.Properties[field.Key],
                field.Value, descriptor.Typename, field.Key);
            AddReflectedEditorRow(fields,
                InspectorPropertyPresentation.FormatPropertyName(descriptor.Typename, field.Key),
                editor, descriptor.RecordType.Properties[field.Key]);
        }
        return fields;
    }

    static void AddReflectedEditorRow(Grid grid, string label, View editor, PropertyBase descriptor)
    {
        if (descriptor is not RecordProperty and not ListProperty)
        {
            Templates.AddGridRowWithLabel(grid, label, editor, GridLength.Auto);
            return;
        }

        var heading = new Label { Text = label, FontAttributes = FontAttributes.Bold };
        Templates.AddGridRow(grid, heading, GridLength.Auto);
        Grid.SetColumnSpan(heading, grid.ColumnDefinitions.Count);
        Templates.AddGridRow(grid, editor, GridLength.Auto);
        Grid.SetColumnSpan(editor, grid.ColumnDefinitions.Count);
    }

    static View CreateValueListEditor(
        Component component, EngineTypes engineTypes, ListProperty descriptor,
        ObservableValueList list, string ownerType, string propertyName)
    {
        var rows = new VerticalStackLayout { Spacing = 8, MinimumWidthRequest = 0 };
        var add = new Button { Text = "Add", HorizontalOptions = LayoutOptions.Start };
        var layout = new VerticalStackLayout { Spacing = 4, Children = { rows, add } };

        Button ActionButton(string text, Action mutation) => new()
        {
            Text = text,
            Padding = new Thickness(8, 2),
            Command = CreateContextMenuCommand(
                () => component.ApplyInspectorBatchAsync(mutation), $"Edit {propertyName}")
        };

        void RebuildRows()
        {
            rows.Children.Clear();
            for (var i = 0; i < list.Values.Count; ++i)
            {
                var index = i;
                var up = ActionButton("Up", () => list.Values.Move(index, index - 1));
                var down = ActionButton("Down", () => list.Values.Move(index, index + 1));
                up.IsEnabled = i > 0;
                down.IsEnabled = i + 1 < list.Values.Count;
                rows.Children.Add(new VerticalStackLayout
                {
                    Spacing = 4,
                    Children =
                    {
                        new HorizontalStackLayout
                        {
                            Spacing = 4,
                            Children =
                            {
                                new Label { Text = (i + 1).ToString(), VerticalOptions = LayoutOptions.Center },
                                up, down, ActionButton("Remove", () => list.Values.RemoveAt(index))
                            }
                        },
                        CreateReflectedValueEditor(
                            component, engineTypes, descriptor.ElementType, list.Values[i], ownerType, propertyName)
                    }
                });
            }
        }

        add.Command = CreateContextMenuCommand(
            () => component.ApplyInspectorBatchAsync(() =>
                list.Values.Add(new ReflectedValueCodec(engineTypes).CreateDefault(descriptor.ElementType))),
            $"Add {propertyName}");
        void OnCollectionChanged(object sender, NotifyCollectionChangedEventArgs args) => RebuildRows();
        layout.Loaded += (_, _) =>
        {
            list.Values.CollectionChanged -= OnCollectionChanged;
            list.Values.CollectionChanged += OnCollectionChanged;
            RebuildRows();
        };
        layout.Unloaded += (_, _) => list.Values.CollectionChanged -= OnCollectionChanged;
        RebuildRows();
        return layout;
    }
}
