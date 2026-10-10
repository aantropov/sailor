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

        var saveVegetation = new Button
        {
            Text = "Save Vegetation"
        };
        ToolTipProperties.SetText(
            saveVegetation,
            "Create or update the binary vegetation asset referenced by this Landscape");
        saveVegetation.Clicked += async (_, _) =>
        {
            saveVegetation.IsEnabled = false;
            var previousText = saveVegetation.Text;
            saveVegetation.Text = "Saving…";
            try
            {
                await SaveLandscapeVegetationAsync(component);
            }
            finally
            {
                saveVegetation.Text = previousText;
                saveVegetation.IsEnabled = true;
            }
        };

        var toolbar = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Star),
                new ColumnDefinition(GridLength.Star)
            },
            ColumnSpacing = 6
        };
        toolbar.Add(generate, 0, 0);
        toolbar.Add(regenerate, 1, 0);
        toolbar.Add(flatten, 2, 0);
        toolbar.Add(saveVegetation, 3, 0);
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

    static async Task SaveLandscapeVegetationAsync(Component component)
    {
        var shell = MauiProgram.GetService<EditorShellHost>();
        if (!component.OverrideProperties.TryGetValue(
                "vegetation",
                out var vegetationProperty) ||
            vegetationProperty is not Observable<FileId> vegetation)
        {
            shell.SetStatus("Landscape vegetation property is unavailable.");
            return;
        }

        if (vegetation.Value is null || vegetation.Value.IsEmpty())
        {
            var world = MauiProgram.GetService<WorldService>();
            var ownerName = world.FindOwner(component)?.Name ?? "Landscape";
            var created = await MauiProgram.GetService<AssetsService>()
                .CreateLandscapeVegetationAssetAsync(ownerName);
            if (created?.FileId is null || created.FileId.IsEmpty())
            {
                shell.SetStatus("Failed to create the Landscape vegetation asset.");
                return;
            }

            var linked = await component.ApplyInspectorBatchAsync(() =>
                vegetation.Value = new FileId(created.FileId.Value));
            if (!linked)
            {
                shell.SetStatus("Vegetation asset was created, but the Landscape link failed.");
                return;
            }
        }

        if (!component.OverrideProperties.TryGetValue(
                "saveVegetation",
                out var actionProperty) ||
            actionProperty is not Observable<bool> action)
        {
            shell.SetStatus("Landscape Save Vegetation action is unavailable.");
            return;
        }

        var requested = await component.ApplyInspectorBatchAsync(() =>
            action.Value = true);
        await component.ApplyInspectorBatchAsync(() =>
            action.Value = false);
        if (!requested)
        {
            shell.SetStatus("Failed to request the Landscape vegetation save.");
            return;
        }
        shell.SetStatus($"Vegetation save requested for {vegetation.Value.Value}.");
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

}
