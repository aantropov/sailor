namespace SailorEditor
{
    public partial class AppShell : Microsoft.Maui.Controls.Shell
    {
        public AppShell()
        {
            InitializeComponent();
#if MACCATALYST
            Title = string.Empty;
#endif
        }
    }
}
