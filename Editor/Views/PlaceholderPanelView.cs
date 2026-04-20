namespace SailorEditor.Views;

using Microsoft.Maui.Controls.Shapes;

public sealed class PlaceholderPanelView : ContentView
{
    public PlaceholderPanelView(string title, string description)
    {
        Content = new Border
        {
            StrokeThickness = 1,
            StrokeShape = new RoundRectangle { CornerRadius = 6 },
            Margin = new Thickness(8),
            Padding = new Thickness(12),
            Content = new VerticalStackLayout
            {
                Spacing = 8,
                Children =
                {
                    new Label { Text = title, FontAttributes = FontAttributes.Bold, FontSize = 16 },
                    new Label { Text = description, Opacity = 0.75 }
                }
            }
        };
    }
}
