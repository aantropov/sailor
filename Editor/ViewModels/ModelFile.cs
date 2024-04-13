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
using SailorEditor.Utility;

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
        private ObservableList<Observable<string>> defaultMaterials = new();

        protected override async Task UpdateModel()
        {
            Properties["bShouldGenerateMaterials"] = ShouldGenerateMaterials;
            Properties["bShouldBatchByMaterial"] = ShouldBatchByMaterial;
            Properties["materials"] = DefaultMaterials.Select((el) => el.Value).ToList();

            IsDirty = false;
        }

        public override async Task<bool> PreloadResources(bool force)
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
                            case "materials":
                                {
                                    var parsed = new ObservableList<Observable<AssetUID>>();
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

                                    DefaultMaterials = parsed;
                                }
                                break;
                        }
                    }

                    DefaultMaterials.CollectionChanged += (a, e) => MarkDirty(nameof(DefaultMaterials));
                    DefaultMaterials.ItemChanged += (a, e) => MarkDirty(nameof(DefaultMaterials));
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