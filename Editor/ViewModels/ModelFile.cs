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
using System.Collections.ObjectModel;
using CommunityToolkit.Maui.Core.Extensions;

namespace SailorEditor.ViewModels
{
    using AssetUID = string;
    public partial class ModelFile : AssetFile
    {
        [ObservableProperty]
        private bool shouldGenerateMaterials;

        [ObservableProperty]
        private bool shouldBatchByMaterial;

        [ObservableProperty]
        private ObservableCollection<AssetUID> defaultMaterials = new();
        protected override void UpdateModel()
        {
            Properties["bShouldGenerateMaterials"] = ShouldGenerateMaterials.ToString();
            Properties["bShouldBatchByMaterial"] = ShouldBatchByMaterial.ToString();
            Properties["defaultMaterials"] = DefaultMaterials.ToString();

            defaultMaterials.CollectionChanged += (s, args) => { OnPropertyChanged(nameof(DefaultMaterials)); };

            IsDirty = false;
        }
        public override bool PreloadResources(bool force)
        {
            if (!IsLoaded || force)
            {
                try
                {
                    foreach (var e in Properties)
                    {
                        switch (e.Key.ToString())
                        {
                            case "bShouldGenerateMaterials":
                                ShouldGenerateMaterials = bool.Parse(e.Value.ToString());
                                break;
                            case "bShouldBatchByMaterial":
                                ShouldBatchByMaterial = bool.Parse(e.Value.ToString());
                                break;
                            case "defaultMaterials":
                                {
                                    var parsed = new List<AssetUID>();
                                    bool ignoreFirst = false;
                                    foreach (var uid in (e.Value as YamlNode).AllNodes)
                                    {
                                        if (!ignoreFirst)
                                        {
                                            ignoreFirst = true;
                                            continue;
                                        }

                                        parsed.Add((AssetUID)uid.ToString());
                                    }

                                    DefaultMaterials = parsed.ToObservableCollection();
                                }
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