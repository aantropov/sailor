using SailorEditor.Utility;
using MauiGrid = Microsoft.Maui.Controls.Grid;

namespace SailorEditor.Views;

public sealed class EditorPerformanceView : ContentView
{
    readonly VerticalStackLayout _rows = new()
    {
        Spacing = 2,
        Padding = new Thickness(8, 6)
    };

    public EditorPerformanceView()
    {
        BackgroundColor = Color.FromArgb("#17181B");
        Content = new ScrollView
        {
            Content = _rows,
            BackgroundColor = Colors.Transparent
        };

        Refresh();
        Dispatcher.StartTimer(TimeSpan.FromSeconds(1), () =>
        {
            if (Handler is null)
                return false;

            Refresh();
            return true;
        });
    }

    void Refresh()
    {
        var samples = EditorPerf.Snapshot()
            .Reverse()
            .Take(80)
            .ToArray();

        _rows.Children.Clear();
        _rows.Children.Add(new Label
        {
            Text = samples.Length == 0 ? "No editor performance samples yet." : "Recent editor scopes",
            FontSize = 12,
            FontAttributes = FontAttributes.Bold,
            TextColor = Color.FromArgb("#D8DADE"),
            Margin = new Thickness(0, 0, 0, 6)
        });

        foreach (var sample in samples)
        {
            var row = new MauiGrid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = GridLength.Star },
                    new ColumnDefinition { Width = GridLength.Auto }
                },
                ColumnSpacing = 12,
                Padding = new Thickness(0, 2),
                BackgroundColor = Colors.Transparent
            };

            row.Children.Add(new Label
            {
                Text = sample.Name,
                FontSize = 11,
                TextColor = Color.FromArgb("#C7C9CE"),
                LineBreakMode = LineBreakMode.TailTruncation,
                MaxLines = 1,
                VerticalTextAlignment = TextAlignment.Center
            });

            var duration = new Label
            {
                Text = $"{sample.DurationMs:F2} ms",
                FontSize = 11,
                TextColor = sample.DurationMs >= 16.0 ? Color.FromArgb("#FFCF6B") : Color.FromArgb("#8F949C"),
                VerticalTextAlignment = TextAlignment.Center,
                HorizontalTextAlignment = TextAlignment.End
            };
            row.Children.Add(duration);
            row.SetColumn(duration, 1);

            _rows.Children.Add(row);
        }
    }
}
