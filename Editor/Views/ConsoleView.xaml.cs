using SailorEditor.Services;
using System.Collections.ObjectModel;
using SailorEditor.AI;

namespace SailorEditor.Views
{
    public partial class ConsoleView : ContentView
    {
        private const int MaxMessages = 1000;

        public ObservableCollection<string> MessageQueue { get; } = new();

        public ConsoleView()
        {
            InitializeComponent();
            BindingContext = this;

            var engineService = MauiProgram.GetService<EngineService>();
            OnUpdateMessageQueue(engineService.GetRecentConsoleMessages());
            engineService.OnPullMessagesAction += OnUpdateMessageQueue;
            MauiProgram.GetService<AIOperatorService>().AuditTrail.CollectionChanged += OnAiAuditChanged;
            Unloaded += OnUnloaded;
        }

        public void OnUpdateMessageQueue(string[] messages)
        {
            if (!MainThread.IsMainThread)
            {
                MainThread.BeginInvokeOnMainThread(() => OnUpdateMessageQueue(messages));
                return;
            }

            foreach (var el in messages)
            {
                if (!string.IsNullOrEmpty(el))
                {
                    MessageQueue.Add(el);
                }
            }

            while (MessageQueue.Count > MaxMessages)
            {
                MessageQueue.RemoveAt(0);
            }

            _ = ScrollToLastMessage();
        }

        private void OnAiAuditChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
        {
            if (e.NewItems is null)
                return;

            var messages = e.NewItems
                .OfType<AIActionAuditEntry>()
                .Select(x => $"[AI] {x.State} • {x.Title} • {x.Summary ?? string.Join(", ", x.Items.Select(i => i.CommandName))}")
                .ToArray();

            OnUpdateMessageQueue(messages);
        }

        private async Task ScrollToLastMessage()
        {
            if (MessageQueue.Count == 0)
                return;

            await Task.Yield();
            await Task.Delay(16);

            MessagesCollection.ScrollTo(MessageQueue.Count - 1, position: ScrollToPosition.End, animate: false);
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

        private void OnUnloaded(object sender, EventArgs e)
        {
            MauiProgram.GetService<EngineService>().OnPullMessagesAction -= OnUpdateMessageQueue;
            MauiProgram.GetService<AIOperatorService>().AuditTrail.CollectionChanged -= OnAiAuditChanged;
            Unloaded -= OnUnloaded;
        }
    }
}
