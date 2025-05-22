using SailorEditor.Helpers;
using SailorEditor.Styles;
using SailorEditor.Services;
using System.Diagnostics;
using System.Windows.Input;

namespace SailorEditor
{
    public partial class MainPage : ContentPage
    {
        public ICommand ChangeThemeCommand => new Command<string>(ChangeTheme);

        public MainPage()
        {
            InitializeComponent();
        }

        private async void OnCreateGameProjectClicked(object sender, EventArgs e)
        {
            await Navigation.PushModalAsync(new Views.CreateGameProjectPage());
        }

        private void ChangeTheme(string theme)
        {
            var mergedDictionaries = Application.Current.Resources.MergedDictionaries;
            mergedDictionaries.Clear();

            switch (theme)
            {
                case "LightThemeStyle":
                    mergedDictionaries.Add(new LightThemeStyle());
                    break;
                case "DarkThemeStyle":
                    mergedDictionaries.Add(new DarkThemeStyle());
                    break;
            }
        }
    }
}