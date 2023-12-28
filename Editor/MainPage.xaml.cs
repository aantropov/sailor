using System.Diagnostics;

namespace Editor
{
    public partial class MainPage : ContentPage
    {
        int count = 0;

        public MainPage()
        {
            InitializeComponent();
        }

        private void OnCounterClicked(object sender, EventArgs e)
        {

            ProcessStartInfo startInfo = new ProcessStartInfo
            {
                FileName = Directory.GetCurrentDirectory() + "\\SailorEngine-Release.exe",
                Arguments = "",
                UseShellExecute = false
            };

            try
            {
                Process process = new Process
                {
                    StartInfo = startInfo
                };
                process.Start();
            }
            catch (Exception ex)
            {
                // Обработка исключения
                Console.WriteLine($"Ошибка при запуске процесса: {ex.Message}");
            }

            count++;

            if (count == 1)
                CounterBtn.Text = $"Clicked {count} time";
            else
                CounterBtn.Text = $"Clicked {count} times";

            SemanticScreenReader.Announce(CounterBtn.Text);
        }
    }
}