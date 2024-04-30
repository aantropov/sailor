using Microsoft.Maui.Controls.Compatibility;
using SailorEditor.Engine;
using SailorEditor.Helpers;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Services;

namespace SailorEditor.ViewModels
{
    public class ShaderLibraryFile : AssetFile
    {
        public string Code { get; set; }

        public override async Task<bool> PreloadResources(bool force)
        {
            if (IsLoaded && !force)
                return true;

            Code = File.ReadAllText(Asset.FullName);

            IsLoaded = true;
            return true;
        }
    }
}