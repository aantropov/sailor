using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace SailorEditor.Views
{
    public partial class ConsoleView : ContentView
    {
        public ObservableList<Observable<string>> MessageQueue { get; private set; } = new ObservableList<Observable<string>>();

        public ConsoleView()
        {
            InitializeComponent();
            BindingContext = this;

            SailorEngine.OnPullMessagesAction += OnUpdateMessageQueue;
        }

        public void OnUpdateMessageQueue(string[] messages)
        {
            foreach (var el in messages)
                MessageQueue.Add(el);

            _ = UpdateMessagesStack();
        }

        private async Task UpdateMessagesStack()
        {
            MessagesStack.Children.Clear();
            foreach (var message in MessageQueue)
            {
                MessagesStack.Children.Add(new Label { Text = message });
            }

            if (MessagesStack.Children.Count > 0)
            {
                // Wait while layout is complete
                await Task.Delay(100);

                var lastChild = MessagesStack.Children[MessagesStack.Children.Count - 1];
                await ScrollView.ScrollToAsync((Element)lastChild, ScrollToPosition.End, true);
            }
        }

        private void OnLabelTapped(object sender, EventArgs e)
        {
            if (sender is Label label)
            {
                var text = label.Text;
                Clipboard.SetTextAsync(text);
                DisplayAlert("Copied", "Text copied to clipboard", "OK");
            }
        }

        private async void DisplayAlert(string title, string message, string cancel)
        {
            // Display alert to confirm the text has been copied
            await Application.Current.MainPage.DisplayAlert(title, message, cancel);
        }
    }
}