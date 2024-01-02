using Microsoft.Maui.Controls;

public class MainPage : ContentPage
{
    Grid mainGrid;
    double initialWidth;

    public MainPage()
    {
        mainGrid = new Grid
        {
            RowDefinitions =
            {
                new RowDefinition { Height = GridLength.Star },
                new RowDefinition { Height = new GridLength(100) }
            },
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = new GridLength(3, GridUnitType.Star) },
                new ColumnDefinition { Width = GridLength.Auto }, // Разделитель
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                new ColumnDefinition { Width = GridLength.Auto }, // Разделитель
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                new ColumnDefinition { Width = GridLength.Auto }, // Разделитель
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) }
            }
        };

        // Добавление содержимого для колонок
        AddContentToGrid();

        // Добавление разделителей
        AddDraggableSeparators();

        this.Content = mainGrid;
    }

    private void AddContentToGrid()
    {
        // SceneView
        var sceneView = new ContentView
        {
            BackgroundColor = Colors.Gray,
            Content = new Label { Text = "SceneView/GameView" }
        };
        mainGrid.Children.Add(sceneView);
        Grid.SetColumn(sceneView, 0);
        Grid.SetRow(sceneView, 0);

        // SceneHierarchy
        var sceneHierarchy = new ContentView
        {
            BackgroundColor = Colors.LightGray,
            Content = new Label { Text = "SceneHierarchy" }
        };
        mainGrid.Children.Add(sceneHierarchy);
        Grid.SetColumn(sceneHierarchy, 2);
        Grid.SetRow(sceneHierarchy, 0);

        // Folder
        var folder = new ContentView
        {
            BackgroundColor = Colors.LightBlue,
            Content = new Label { Text = "Folder" }
        };
        mainGrid.Children.Add(folder);
        Grid.SetColumn(folder, 4);
        Grid.SetRow(folder, 0);

        // Inspector
        var inspector = new ContentView
        {
            BackgroundColor = Colors.LightGreen,
            Content = new Label { Text = "Inspector" }
        };
        mainGrid.Children.Add(inspector);
        Grid.SetColumn(inspector, 6);
        Grid.SetRow(inspector, 0);

        // Обеспечиваем, что каждая колонка занимает одну строку
        Grid.SetRowSpan(sceneView, 2);
        Grid.SetRowSpan(sceneHierarchy, 2);
        Grid.SetRowSpan(folder, 2);
        Grid.SetRowSpan(inspector, 2);
    }
    private void AddDraggableSeparators()
    {
        for (int i = 1; i < mainGrid.ColumnDefinitions.Count; i += 2)
        {
            var separator = new BoxView { BackgroundColor = Colors.Black, WidthRequest = 5 };
            var panGesture = new PanGestureRecognizer();
            panGesture.PanUpdated += (sender, e) => OnPanUpdated(sender, e, i - 1);
            separator.GestureRecognizers.Add(panGesture);

            mainGrid.Children.Add(separator);
            Grid.SetColumn(separator, i);
            Grid.SetRowSpan(separator, 2);
        }
    }
    private void OnPanUpdated(object sender, PanUpdatedEventArgs e, int columnIndex)
    {
        if (e.StatusType == GestureStatus.Started)
        {
            initialWidth = mainGrid.ColumnDefinitions[columnIndex].Width.Value;
        }
        else if (e.StatusType == GestureStatus.Running)
        {
            double newWidth = initialWidth + e.TotalX;

            // Установка минимальной и максимальной ширины для колонок
            const double minWidth = 50.0;
            double maxWidth = mainGrid.Width - minWidth * (mainGrid.ColumnDefinitions.Count - 1);

            newWidth = Math.Max(minWidth, newWidth);
            newWidth = Math.Min(maxWidth, newWidth);

            // Обновляем ширину текущей колонки
            mainGrid.ColumnDefinitions[columnIndex].Width = new GridLength(newWidth, GridUnitType.Absolute);

            // Если есть следующая колонка, обновляем и её ширину
            if (columnIndex + 2 < mainGrid.ColumnDefinitions.Count)
            {
                double nextColumnWidth = mainGrid.ColumnDefinitions[columnIndex + 2].Width.Value - e.TotalX;
                nextColumnWidth = Math.Max(minWidth, nextColumnWidth);

                mainGrid.ColumnDefinitions[columnIndex + 2].Width = new GridLength(nextColumnWidth, GridUnitType.Absolute);
            }
        }
    }

}
