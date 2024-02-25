using Microsoft.Maui.Controls.Compatibility;
using SailorEditor.Engine;
using SailorEditor.Helpers;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Services;
using CommunityToolkit.Mvvm.ComponentModel;

namespace SailorEditor.ViewModels
{
    using AssetUID = string;
    public partial class TextureFile : AssetFile
    {
        public bool IsImageLoaded { get => !Texture.IsEmpty; }

        [ObservableProperty]
        private TextureFiltration filtration;

        [ObservableProperty]
        private TextureFormat format = TextureFormat.R8G8B8A8_SRGB;

        [ObservableProperty]
        private TextureClamping clamping;

        [ObservableProperty]
        private bool shouldGenerateMips;

        [ObservableProperty]
        private bool shouldSupportStorageBinding;

        [ObservableProperty]
        private ImageSource texture;

        protected override void UpdateModel()
        {
            Properties["bShouldGenerateMips"] = ShouldGenerateMips.ToString();
            Properties["bShouldSupportStorageBinding"] = ShouldSupportStorageBinding.ToString();
            Properties["clamping"] = Clamping.ToString();
            Properties["filtration"] = Filtration.ToString();
            Properties["format"] = Format.ToString();

            IsDirty = false;
        }
        
        public override bool PreloadResources(bool force)
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
                                {
                                    TextureClamping outEnum;
                                    if (Enum.TryParse(e.Value.ToString(), out outEnum))
                                        Clamping = outEnum;
                                    else
                                        return false;
                                    break;
                                }
                            case "filtration":
                                {
                                    TextureFiltration outEnum;
                                    if (Enum.TryParse(e.Value.ToString(), out outEnum))
                                        Filtration = outEnum;
                                    else
                                        return false;
                                    break;
                                }
                            case "format":
                                {
                                    TextureFormat outEnum;
                                    if (Enum.TryParse(e.Value.ToString(), out outEnum))
                                        Format = outEnum;
                                    else
                                        return false;
                                    break;
                                }
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