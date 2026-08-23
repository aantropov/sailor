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

public partial class ComponentTemplate
{
    static void AddLandscapeTools(Grid props, Component component)
    {
        var heading = new Label
        {
            Text = "Landscape Tools",
            FontAttributes = FontAttributes.Bold,
            Margin = new Thickness(0, 8, 0, 0)
        };
        Templates.AddGridRow(props, heading, GridLength.Auto);
        Grid.SetColumnSpan(heading, props.ColumnDefinitions.Count);

        var generate = new Button
        {
            Text = "Generate"
        };
        ToolTipProperties.SetText(generate, "Generate terrain from the next seed");
        generate.Clicked += async (_, _) =>
        {
            generate.IsEnabled = false;
            var previousText = generate.Text;
            generate.Text = "Generating…";
            try
            {
                await RebuildLandscapeAsync(component, advanceSeed: true);
            }
            finally
            {
                generate.Text = previousText;
                generate.IsEnabled = true;
            }
        };

        var regenerate = new Button
        {
            Text = "Regenerate"
        };
        ToolTipProperties.SetText(regenerate, "Rebuild terrain and vegetation from the current authored settings");
        regenerate.Clicked += async (_, _) =>
        {
            regenerate.IsEnabled = false;
            var previousText = regenerate.Text;
            regenerate.Text = "Rebuilding…";
            try
            {
                await RebuildLandscapeAsync(component, advanceSeed: false);
            }
            finally
            {
                regenerate.Text = previousText;
                regenerate.IsEnabled = true;
            }
        };

        var flatten = new Button
        {
            Text = "Flatten"
        };
        ToolTipProperties.SetText(flatten, "Set the generated terrain height to zero");
        flatten.Clicked += async (_, _) =>
        {
            if (component.OverrideProperties.TryGetValue("heightScale", out var value) &&
                value is Observable<float> heightScale)
            {
                heightScale.Value = 0.0f;
                await RebuildLandscapeAsync(component, advanceSeed: false);
            }
        };

        var toolbar = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Star)
            },
            ColumnSpacing = 6
        };
        toolbar.Add(generate, 0, 0);
        toolbar.Add(regenerate, 1, 0);
        toolbar.Add(flatten, 2, 0);
        Templates.AddGridRow(props, toolbar, GridLength.Auto);
        Grid.SetColumnSpan(toolbar, props.ColumnDefinitions.Count);

        var hint = new Label
        {
            Text = "Author terrain shape, layer painting and vegetation profiles through the Sailor landscape MCP tools.",
            TextColor = Color.FromArgb("#929AA5"),
            FontSize = 11
        };
        Templates.AddGridRow(props, hint, GridLength.Auto);
        Grid.SetColumnSpan(hint, props.ColumnDefinitions.Count);
    }

    static async Task RebuildLandscapeAsync(Component component, bool advanceSeed)
    {
        if (advanceSeed &&
            component.OverrideProperties.TryGetValue("seed", out var seedProperty) &&
            seedProperty is Observable<uint> seed)
        {
            seed.Value = (uint)Random.Shared.NextInt64(0, (long)uint.MaxValue + 1L);
            await component.CommitInspectorChangesAsync();
            return;
        }

        if (!component.OverrideProperties.TryGetValue("regenerate", out var regenerateProperty) ||
            regenerateProperty is not Observable<bool> regenerate)
        {
            await component.CommitInspectorChangesAsync();
            return;
        }

        regenerate.Value = true;
        await component.CommitInspectorChangesAsync();
        regenerate.Value = false;
        await component.CommitInspectorChangesAsync();
    }

    static void AddLandscapeVegetationEditor(Grid props, Component component)
    {
        if (!TryGetLandscapeVegetationLists(component,
                out var models,
                out var materials,
                out var meshIndex,
                out var instancesPerChunk,
                out var residency,
                out var priority,
                out var minScale,
                out var maxScale,
                out var groundOffset,
                out var shadowMode,
                out var shadowDistance,
                out var minLod,
                out var maxLod,
                out var lod1ScreenCoverage,
                out var lod2ScreenCoverage,
                out var cullDistance,
                out var colliderRadius,
                out var colliderHeight,
                out var colliderOffsetY))
        {
            return;
        }

        var headingRow = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Auto)
            },
            ColumnSpacing = 6,
            Margin = new Thickness(0, 8, 0, 0)
        };
        headingRow.Add(new Label
        {
            Text = "Vegetation",
            FontAttributes = FontAttributes.Bold,
            VerticalOptions = LayoutOptions.Center
        }, 0, 0);
        var addProfile = new Button { Text = "+ Add" };
        headingRow.Add(addProfile, 1, 0);
        Templates.AddGridRow(props, headingRow, GridLength.Auto);
        Grid.SetColumnSpan(headingRow, props.ColumnDefinitions.Count);

        var profiles = new VerticalStackLayout
        {
            Spacing = 8,
            HorizontalOptions = LayoutOptions.Fill
        };
        Templates.AddGridRow(props, profiles, GridLength.Auto);
        Grid.SetColumnSpan(profiles, props.ColumnDefinitions.Count);

        void RebuildProfiles()
        {
            profiles.Children.Clear();
            var count = new[]
            {
                models.Values.Count,
                materials.Values.Count,
                meshIndex.Values.Count,
                instancesPerChunk.Values.Count,
                residency.Values.Count,
                priority.Values.Count,
                minScale.Values.Count,
                maxScale.Values.Count,
                groundOffset.Values.Count,
                shadowMode.Values.Count,
                shadowDistance.Values.Count,
                minLod.Values.Count,
                maxLod.Values.Count,
                lod1ScreenCoverage.Values.Count,
                lod2ScreenCoverage.Values.Count,
                cullDistance.Values.Count,
                colliderRadius.Values.Count,
                colliderHeight.Values.Count,
                colliderOffsetY.Values.Count,
            }.Min();

            if (count == 0)
            {
                profiles.Children.Add(new Label
                {
                    Text = "No vegetation profiles. Add one or author the list through MCP.",
                    TextColor = Color.FromArgb("#929AA5"),
                    FontSize = 11
                });
                return;
            }

            for (var profileIndex = 0; profileIndex < count; ++profileIndex)
            {
                var index = profileIndex;
                var card = new Grid
                {
                    ColumnDefinitions =
                    {
                        new ColumnDefinition(new GridLength(110)),
                        new ColumnDefinition(GridLength.Star)
                    },
                    RowDefinitions =
                    {
                        new RowDefinition(GridLength.Auto)
                    },
                    RowSpacing = 5,
                    ColumnSpacing = 8,
                    Padding = new Thickness(8),
                    BackgroundColor = Color.FromArgb("#141A22")
                };

                var title = new Label
                {
                    Text = $"Profile {index + 1}",
                    FontAttributes = FontAttributes.Bold,
                    VerticalOptions = LayoutOptions.Center
                };
                card.Add(title, 0, 0);
                var remove = new Button
                {
                    Text = "Remove",
                    HorizontalOptions = LayoutOptions.End
                };
                remove.Clicked += async (_, _) =>
                {
                    remove.IsEnabled = false;
                    await component.ApplyInspectorBatchAsync(() =>
                    {
                        RemoveLandscapeVegetationProfile(index, models, materials,
                            meshIndex, instancesPerChunk, residency, priority,
                            minScale, maxScale, groundOffset,
                            shadowMode, shadowDistance, minLod, maxLod,
                            lod1ScreenCoverage, lod2ScreenCoverage, cullDistance,
                            colliderRadius, colliderHeight, colliderOffsetY);
                        RebuildProfiles();
                    });
                };
                card.Add(remove, 1, 0);

                AddVegetationField(card, "Model", Templates.FileIdEditor(
                    models.Values[index],
                    nameof(Observable<FileId>.Value),
                    static (Observable<FileId> value) => value.Value,
                    static (value, fileId) => value.Value = fileId,
                    typeof(ModelFile)));
                AddVegetationField(card, "Material override", Templates.FileIdEditor(
                    materials.Values[index],
                    nameof(Observable<FileId>.Value),
                    static (Observable<FileId> value) => value.Value,
                    static (value, fileId) => value.Value = fileId,
                    typeof(MaterialFile)));
                AddVegetationField(card, "Mesh index", CreateLandscapeFloatEditor(
                    component, meshIndex.Values[index], value => MathF.Max(-1.0f, MathF.Round(value))));
                AddVegetationField(card, "Per chunk", CreateLandscapeFloatEditor(
                    component, instancesPerChunk.Values[index], value => MathF.Max(0.0f, MathF.Round(value))));
                var residencyPicker = new Picker
                {
                    ItemsSource = new[] { "Persistent", "Grass (budgeted)" },
                    SelectedIndex = Math.Clamp((int)residency.Values[index].Value, 0, 1),
                    FontSize = 12,
                    BindingContext = component
                };
                residencyPicker.SelectedIndexChanged += (_, _) =>
                {
                    if (residencyPicker.SelectedIndex < 0)
                        return;
                    residency.Values[index].Value = residencyPicker.SelectedIndex;
                    _ = component.CommitInspectorChangesAsync();
                };
                AddVegetationField(card, "Residency", residencyPicker);
                AddVegetationField(card, "Priority", CreateLandscapeFloatEditor(
                    component, priority.Values[index], value => Math.Clamp(value, 0.0f, 100.0f)));
                AddVegetationField(card, "Min scale", CreateLandscapeFloatEditor(
                    component, minScale.Values[index], value => MathF.Max(0.01f, value)));
                AddVegetationField(card, "Max scale", CreateLandscapeFloatEditor(
                    component, maxScale.Values[index], value => MathF.Max(minScale.Values[index].Value, value)));
                AddVegetationField(card, "Ground offset", CreateLandscapeFloatEditor(
                    component, groundOffset.Values[index], value => value));

                var shadows = new Picker
                {
                    ItemsSource = new[] { "None", "Near only", "All" },
                    SelectedIndex = Math.Clamp((int)shadowMode.Values[index].Value, 0, 2),
                    FontSize = 12,
                    BindingContext = component
                };
                shadows.SelectedIndexChanged += (_, _) =>
                {
                    if (shadows.SelectedIndex < 0)
                        return;
                    shadowMode.Values[index].Value = shadows.SelectedIndex;
                    _ = component.CommitInspectorChangesAsync();
                };
                AddVegetationField(card, "Shadows", shadows);
                AddVegetationField(card, "Near distance", CreateLandscapeFloatEditor(
                    component, shadowDistance.Values[index], value => MathF.Max(0.1f, value)));
                AddVegetationField(card, "Min LOD", CreateLandscapeFloatEditor(
                    component, minLod.Values[index], value => Math.Clamp(MathF.Round(value), 0.0f, 15.0f)));
                AddVegetationField(card, "Max LOD", CreateLandscapeFloatEditor(
                    component, maxLod.Values[index], value => Math.Clamp(MathF.Round(value), minLod.Values[index].Value, 15.0f)));
                AddVegetationField(card, "LOD 1 coverage", CreateLandscapeFloatEditor(
                    component, lod1ScreenCoverage.Values[index], value => Math.Clamp(value, lod2ScreenCoverage.Values[index].Value, 1.0f)));
                AddVegetationField(card, "LOD 2 coverage", CreateLandscapeFloatEditor(
                    component, lod2ScreenCoverage.Values[index], value => Math.Clamp(value, 0.0f, lod1ScreenCoverage.Values[index].Value)));
                AddVegetationField(card, "Cull distance", CreateLandscapeFloatEditor(
                    component, cullDistance.Values[index], value => MathF.Max(0.1f, value)));
                AddVegetationField(card, "Collider radius", CreateLandscapeFloatEditor(
                    component, colliderRadius.Values[index], value => MathF.Max(0.0f, value)));
                AddVegetationField(card, "Collider height", CreateLandscapeFloatEditor(
                    component, colliderHeight.Values[index], value => MathF.Max(colliderRadius.Values[index].Value * 2.0f, value)));
                AddVegetationField(card, "Collider Y offset", CreateLandscapeFloatEditor(
                    component, colliderOffsetY.Values[index], value => value));

                profiles.Children.Add(card);
            }
        }

        addProfile.Clicked += async (_, _) =>
        {
            addProfile.IsEnabled = false;
            try
            {
                await component.ApplyInspectorBatchAsync(() =>
                {
                    models.Values.Add(new Observable<FileId>(new FileId()));
                    materials.Values.Add(new Observable<FileId>(new FileId()));
                    meshIndex.Values.Add(new Observable<float>(-1.0f));
                    instancesPerChunk.Values.Add(new Observable<float>(4.0f));
                    residency.Values.Add(new Observable<float>(0.0f));
                    priority.Values.Add(new Observable<float>(1.0f));
                    minScale.Values.Add(new Observable<float>(0.75f));
                    maxScale.Values.Add(new Observable<float>(1.25f));
                    groundOffset.Values.Add(new Observable<float>(0.0f));
                    shadowMode.Values.Add(new Observable<float>(1.0f));
                    shadowDistance.Values.Add(new Observable<float>(35.0f));
                    minLod.Values.Add(new Observable<float>(0.0f));
                    maxLod.Values.Add(new Observable<float>(2.0f));
                    lod1ScreenCoverage.Values.Add(new Observable<float>(0.25f));
                    lod2ScreenCoverage.Values.Add(new Observable<float>(0.05f));
                    cullDistance.Values.Add(new Observable<float>(120.0f));
                    colliderRadius.Values.Add(new Observable<float>(0.0f));
                    colliderHeight.Values.Add(new Observable<float>(2.0f));
                    colliderOffsetY.Values.Add(new Observable<float>(1.0f));
                    RebuildProfiles();
                });
            }
            finally
            {
                addProfile.IsEnabled = true;
            }
        };
        RebuildProfiles();
    }

    static void AddLandscapeImportEditor(Grid props, Component component)
    {
        if (!component.OverrideProperties.TryGetValue("heightmapTexture", out var heightmapProperty) ||
            heightmapProperty is not Observable<FileId> heightmap ||
            !component.OverrideProperties.TryGetValue("layerTextures", out var layersProperty) ||
            layersProperty is not ObservableFileIdList layers ||
            !component.OverrideProperties.TryGetValue("materialMasks", out var masksProperty) ||
            masksProperty is not ObservableFileIdList masks)
        {
            return;
        }

        var heading = new Label
        {
            Text = "Imported Maps",
            FontAttributes = FontAttributes.Bold,
            Margin = new Thickness(0, 8, 0, 0)
        };
        Templates.AddGridRow(props, heading, GridLength.Auto);
        Grid.SetColumnSpan(heading, props.ColumnDefinitions.Count);

        var status = new Label
        {
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5")
        };

        var heightmapRow = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Auto)
            },
            ColumnSpacing = 6
        };
        var heightmapEditor = Templates.FileIdEditor(
            heightmap,
            nameof(Observable<FileId>.Value),
            static (Observable<FileId> value) => value.Value,
            static (value, fileId) => value.Value = fileId,
            typeof(TextureFile));
        heightmapRow.Add(heightmapEditor, 0, 0);
        var importHeightmap = new Button { Text = "Import…" };
        ToolTipProperties.SetText(importHeightmap,
            "Copy an image into Content/Landscape/Heightmaps and use it as terrain height");
        importHeightmap.Clicked += async (_, _) =>
        {
            var picked = await FilePicker.Default.PickAsync(new PickOptions
            {
                PickerTitle = "Import landscape heightmap"
            });
            if (picked is null)
                return;

            status.Text = "Importing heightmap…";
            var fileId = await MauiProgram.GetService<AssetsService>()
                .ImportTextureAsync(picked.FullPath, "Heightmaps");
            if (fileId is null || fileId.IsEmpty())
            {
                status.Text = "Heightmap import failed. Use PNG, JPG, BMP, TGA, DDS or HDR.";
                return;
            }
            heightmap.Value = fileId;
            await component.CommitInspectorChangesAsync();
            status.Text = "Heightmap imported. Regenerate to rebuild the terrain.";
        };
        heightmapRow.Add(importHeightmap, 1, 0);
        Templates.AddGridRowWithLabel(props, "Heightmap", heightmapRow, GridLength.Auto);

        var layersEditor = new VerticalStackLayout { Spacing = 4 };
        layersEditor.Children.Add(Templates.FileIdListEditor(layers, typeof(TextureFile)));
        layersEditor.Children.Add(new Label
        {
            Text = "Up to four albedo layers. Material masks use the same order.",
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5")
        });
        Templates.AddGridRowWithLabel(props, "Material layers", layersEditor, GridLength.Auto);

        var masksRow = new VerticalStackLayout { Spacing = 6 };
        masksRow.Children.Add(Templates.FileIdListEditor(masks, typeof(TextureFile)));
        var importMasks = new Button
        {
            Text = "Import Material Masks…",
            HorizontalOptions = LayoutOptions.Start
        };
        ToolTipProperties.SetText(importMasks,
            "Import one RGBA splat-map or up to four grayscale masks, ordered by landscape layer");
        importMasks.Clicked += async (_, _) =>
        {
            var pickedFiles = await FilePicker.Default.PickMultipleAsync(new PickOptions
            {
                PickerTitle = "Import landscape material masks"
            });
            var selected = pickedFiles?.Take(4).ToArray() ?? [];
            if (selected.Length == 0)
                return;

            status.Text = "Importing material masks…";
            var imported = new List<FileId>(selected.Length);
            foreach (var picked in selected)
            {
                var fileId = await MauiProgram.GetService<AssetsService>()
                    .ImportTextureAsync(picked.FullPath, "Masks");
                if (fileId is not null && !fileId.IsEmpty())
                {
                    imported.Add(fileId);
                }
            }
            if (imported.Count != selected.Length)
            {
                status.Text = "One or more material masks could not be imported.";
                return;
            }

            masks.Values.Clear();
            foreach (var fileId in imported)
            {
                masks.Values.Add(new Observable<FileId>(fileId));
            }
            await component.CommitInspectorChangesAsync();
            status.Text = imported.Count == 1
                ? "RGBA material mask imported. Regenerate to apply it."
                : $"{imported.Count} material masks imported. Regenerate to apply them.";
        };
        masksRow.Children.Add(importMasks);
        Templates.AddGridRowWithLabel(props, "Material masks", masksRow, GridLength.Auto);
        Templates.AddGridRow(props, status, GridLength.Auto);
        Grid.SetColumnSpan(status, props.ColumnDefinitions.Count);
    }

    static View CreateLandscapeFloatEditor(
        Component component,
        Observable<float> observable,
        Func<float, float> sanitize)
    {
        var editor = Templates.FloatEditor(
            (Component _) => observable.Value,
            (Component _, float value) => observable.Value = sanitize(value));
        editor.BindingContext = component;
        return editor;
    }

    static void AddVegetationField(Grid card, string label, View editor)
    {
        var row = card.RowDefinitions.Count;
        card.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        card.Add(new Label
        {
            Text = label,
            FontSize = 11,
            TextColor = Color.FromArgb("#929AA5"),
            VerticalOptions = LayoutOptions.Center
        }, 0, row);
        editor.HorizontalOptions = LayoutOptions.Fill;
        editor.MinimumWidthRequest = 0;
        card.Add(editor, 1, row);
    }

    static void RemoveLandscapeVegetationProfile(
        int index,
        ObservableFileIdList models,
        ObservableFileIdList materials,
        ObservableFloatList meshIndex,
        ObservableFloatList instancesPerChunk,
        ObservableFloatList residency,
        ObservableFloatList priority,
        ObservableFloatList minScale,
        ObservableFloatList maxScale,
        ObservableFloatList groundOffset,
        ObservableFloatList shadowMode,
        ObservableFloatList shadowDistance,
        ObservableFloatList minLod,
        ObservableFloatList maxLod,
        ObservableFloatList lod1ScreenCoverage,
        ObservableFloatList lod2ScreenCoverage,
        ObservableFloatList cullDistance,
        ObservableFloatList colliderRadius,
        ObservableFloatList colliderHeight,
        ObservableFloatList colliderOffsetY)
    {
        models.Values.RemoveAt(index);
        materials.Values.RemoveAt(index);
        meshIndex.Values.RemoveAt(index);
        instancesPerChunk.Values.RemoveAt(index);
        residency.Values.RemoveAt(index);
        priority.Values.RemoveAt(index);
        minScale.Values.RemoveAt(index);
        maxScale.Values.RemoveAt(index);
        groundOffset.Values.RemoveAt(index);
        shadowMode.Values.RemoveAt(index);
        shadowDistance.Values.RemoveAt(index);
        minLod.Values.RemoveAt(index);
        maxLod.Values.RemoveAt(index);
        lod1ScreenCoverage.Values.RemoveAt(index);
        lod2ScreenCoverage.Values.RemoveAt(index);
        cullDistance.Values.RemoveAt(index);
        colliderRadius.Values.RemoveAt(index);
        colliderHeight.Values.RemoveAt(index);
        colliderOffsetY.Values.RemoveAt(index);
    }

    static bool TryGetLandscapeVegetationLists(
        Component component,
        out ObservableFileIdList models,
        out ObservableFileIdList materials,
        out ObservableFloatList meshIndex,
        out ObservableFloatList instancesPerChunk,
        out ObservableFloatList residency,
        out ObservableFloatList priority,
        out ObservableFloatList minScale,
        out ObservableFloatList maxScale,
        out ObservableFloatList groundOffset,
        out ObservableFloatList shadowMode,
        out ObservableFloatList shadowDistance,
        out ObservableFloatList minLod,
        out ObservableFloatList maxLod,
        out ObservableFloatList lod1ScreenCoverage,
        out ObservableFloatList lod2ScreenCoverage,
        out ObservableFloatList cullDistance,
        out ObservableFloatList colliderRadius,
        out ObservableFloatList colliderHeight,
        out ObservableFloatList colliderOffsetY)
    {
        component.OverrideProperties.TryGetValue("vegetationModels", out var modelsProperty);
        component.OverrideProperties.TryGetValue("vegetationMaterials", out var materialsProperty);
        component.OverrideProperties.TryGetValue("vegetationMeshIndex", out var meshIndexProperty);
        component.OverrideProperties.TryGetValue("vegetationInstancesPerChunk", out var instancesPerChunkProperty);
        component.OverrideProperties.TryGetValue("vegetationResidency", out var residencyProperty);
        component.OverrideProperties.TryGetValue("vegetationPriority", out var priorityProperty);
        component.OverrideProperties.TryGetValue("vegetationMinScale", out var minScaleProperty);
        component.OverrideProperties.TryGetValue("vegetationMaxScale", out var maxScaleProperty);
        component.OverrideProperties.TryGetValue("vegetationGroundOffset", out var groundOffsetProperty);
        component.OverrideProperties.TryGetValue("vegetationShadowMode", out var shadowModeProperty);
        component.OverrideProperties.TryGetValue("vegetationShadowDistance", out var shadowDistanceProperty);
        component.OverrideProperties.TryGetValue("vegetationMinLod", out var minLodProperty);
        component.OverrideProperties.TryGetValue("vegetationMaxLod", out var maxLodProperty);
        component.OverrideProperties.TryGetValue("vegetationLod1ScreenCoverage", out var lod1ScreenCoverageProperty);
        component.OverrideProperties.TryGetValue("vegetationLod2ScreenCoverage", out var lod2ScreenCoverageProperty);
        component.OverrideProperties.TryGetValue("vegetationCullDistance", out var cullDistanceProperty);
        component.OverrideProperties.TryGetValue("vegetationColliderRadius", out var colliderRadiusProperty);
        component.OverrideProperties.TryGetValue("vegetationColliderHeight", out var colliderHeightProperty);
        component.OverrideProperties.TryGetValue("vegetationColliderOffsetY", out var colliderOffsetYProperty);

        models = modelsProperty as ObservableFileIdList;
        materials = materialsProperty as ObservableFileIdList;
        meshIndex = meshIndexProperty as ObservableFloatList;
        instancesPerChunk = instancesPerChunkProperty as ObservableFloatList;
        residency = residencyProperty as ObservableFloatList;
        priority = priorityProperty as ObservableFloatList;
        minScale = minScaleProperty as ObservableFloatList;
        maxScale = maxScaleProperty as ObservableFloatList;
        groundOffset = groundOffsetProperty as ObservableFloatList;
        shadowMode = shadowModeProperty as ObservableFloatList;
        shadowDistance = shadowDistanceProperty as ObservableFloatList;
        minLod = minLodProperty as ObservableFloatList;
        maxLod = maxLodProperty as ObservableFloatList;
        lod1ScreenCoverage = lod1ScreenCoverageProperty as ObservableFloatList;
        lod2ScreenCoverage = lod2ScreenCoverageProperty as ObservableFloatList;
        cullDistance = cullDistanceProperty as ObservableFloatList;
        colliderRadius = colliderRadiusProperty as ObservableFloatList;
        colliderHeight = colliderHeightProperty as ObservableFloatList;
        colliderOffsetY = colliderOffsetYProperty as ObservableFloatList;
        if (models is not null)
        {
            EnsureLandscapeProfileLength(materials, models.Values.Count);
            EnsureLandscapeProfileLength(meshIndex, models.Values.Count, -1.0f);
            EnsureLandscapeProfileLength(residency, models.Values.Count, 0.0f);
            EnsureLandscapeProfileLength(priority, models.Values.Count, 1.0f);
            EnsureLandscapeProfileLength(minLod, models.Values.Count, 0.0f);
            EnsureLandscapeProfileLength(maxLod, models.Values.Count, 2.0f);
            EnsureLandscapeProfileLength(lod1ScreenCoverage, models.Values.Count, 0.25f);
            EnsureLandscapeProfileLength(lod2ScreenCoverage, models.Values.Count, 0.05f);
            EnsureLandscapeProfileLength(cullDistance, models.Values.Count, 120.0f);
            EnsureLandscapeProfileLength(colliderRadius, models.Values.Count, 0.0f);
            EnsureLandscapeProfileLength(colliderHeight, models.Values.Count, 2.0f);
            EnsureLandscapeProfileLength(colliderOffsetY, models.Values.Count, 1.0f);
        }
        return models is not null && materials is not null && meshIndex is not null &&
            instancesPerChunk is not null && residency is not null && priority is not null &&
            minScale is not null &&
            maxScale is not null && groundOffset is not null &&
            shadowMode is not null && shadowDistance is not null &&
            minLod is not null && maxLod is not null &&
            lod1ScreenCoverage is not null && lod2ScreenCoverage is not null &&
            cullDistance is not null && colliderRadius is not null &&
            colliderHeight is not null && colliderOffsetY is not null;
    }

    static void EnsureLandscapeProfileLength(
        ObservableFileIdList? values,
        int count)
    {
        if (values is null)
            return;
        while (values.Values.Count < count)
        {
            values.Values.Add(new Observable<FileId>(new FileId()));
        }
    }

    static void EnsureLandscapeProfileLength(
        ObservableFloatList? values,
        int count,
        float defaultValue)
    {
        if (values is null)
            return;
        while (values.Values.Count < count)
        {
            values.Values.Add(new Observable<float>(defaultValue));
        }
    }

}
