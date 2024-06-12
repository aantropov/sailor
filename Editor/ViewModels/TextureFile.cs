using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEngine;

namespace SailorEditor.ViewModels
{
    public partial class TextureFile : AssetFile
    {
        public bool IsImageLoaded { get => !Texture.IsEmpty; }

        [ObservableProperty]
        private string filtration;

        [ObservableProperty]
        private string format = "R8G8B8A8_SRGB";

        [ObservableProperty]
        private string clamping;

        [ObservableProperty]
        private bool shouldGenerateMips;

        [ObservableProperty]
        private bool shouldSupportStorageBinding;

        [ObservableProperty]
        private ImageSource texture;

        protected override async Task UpdateModel()
        {
            Properties["bShouldGenerateMips"] = ShouldGenerateMips;
            Properties["bShouldSupportStorageBinding"] = ShouldSupportStorageBinding;
            Properties["clamping"] = Clamping;
            Properties["filtration"] = Filtration;
            Properties["format"] = Format;

            IsDirty = false;
        }

        public override async Task<bool> PreloadResources(bool force)
        {
            if (!IsLoaded || force)
            {
                try
                {
                    Texture = ImageSource.FromFile(Asset.FullName);
                }
                catch (Exception ex)
                {
                    DisplayName = ex.Message;
                }

                try
                {
                    foreach (var e in Properties)
                    {
                        switch (e.Key.ToString())
                        {
                            case "bShouldGenerateMips":
                                ShouldGenerateMips = bool.Parse(e.Value.ToString());
                                break;
                            case "bShouldSupportStorageBinding":
                                ShouldSupportStorageBinding = bool.Parse(e.Value.ToString());
                                break;
                            case "clamping":
                                Clamping = e.Value.ToString();
                                break;
                            case "filtration":
                                Filtration = e.Value.ToString();
                                break;
                            case "format":
                                Format = e.Value.ToString();
                                break;
                        }
                    }
                }
                catch (Exception e)
                {
                    DisplayName = e.Message;
                    return false;
                }

                IsDirty = false;
                IsLoaded = true;
            }

            return true;
        }
    }
}