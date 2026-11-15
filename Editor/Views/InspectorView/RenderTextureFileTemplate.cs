using System;
using System.Linq.Expressions;
using Microsoft.Maui.Controls;
using SailorEditor.Utility;
using SailorEditor.Helpers;
using SailorEngine;
using SailorEditor.ViewModels;

namespace SailorEditor;

public sealed class RenderTextureFileTemplate : DataTemplate
{
    public RenderTextureFileTemplate()
    {
        LoadTemplate = static () => CreatePanel();
    }

    private static Grid CreatePanel()
    {
        var grid = new Grid
        {
            RowSpacing = 8,
            ColumnSpacing = 8,
            Padding = 8
        };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var row = 0;

        AddLabelValueRow(grid, ref row, "Width(px)",
            Templates.RangedIntEditor<RenderTextureFile>(
                x => x.RenderWidth,
                (x, v) => x.RenderWidth = v,
                new NumericPropertyRange(1, 16384)));

        AddLabelValueRow(grid, ref row, "Height(px)",
            Templates.RangedIntEditor<RenderTextureFile>(
                x => x.RenderHeight,
                (x, v) => x.RenderHeight = v,
                new NumericPropertyRange(1, 16384)));

        var picker = new Picker();
        picker.SetBinding(Picker.ItemsSourceProperty,
            new Binding(nameof(RenderTextureFile.RenderFormats)));
        picker.SetBinding(Picker.SelectedItemProperty,
            new Binding(nameof(RenderTextureFile.RenderFormat), BindingMode.TwoWay));

        AddLabelValueRow(grid, ref row, "Format", picker);

        grid.BindingContextChanged += (s, e) =>
        {
            if (grid.BindingContext is RenderTextureFile file)
            {
                grid.IsEnabled = !file.IsReadOnly;
            }
        };

        return grid;
    }

    private static void AddLabelValueRow(Grid grid, ref int row, string label, View value)
    {
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.Add(new Label { Text = label, VerticalOptions = LayoutOptions.Center }, 0, row);
        grid.Add(value, 1, row);
        row++;
    }
}
