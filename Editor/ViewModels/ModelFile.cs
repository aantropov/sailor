using YamlDotNet.RepresentationModel;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using System.Globalization;
using SailorEngine;

namespace SailorEditor.ViewModels
{
    public partial class ModelFile : AssetFile
    {
        [ObservableProperty]
        private bool shouldGenerateMaterials;

        [ObservableProperty]
        private bool shouldBatchByMaterial;

        [ObservableProperty]
        private float unitScale;

        [ObservableProperty]
        private ObservableList<Observable<FileId>> defaultMaterials = new();

        protected override async Task UpdateModel()
        {
            Properties["bShouldGenerateMaterials"] = ShouldGenerateMaterials;
            Properties["bShouldBatchByMaterial"] = ShouldBatchByMaterial;
            Properties["unitScale"] = UnitScale;
            Properties["materials"] = DefaultMaterials.Select((el) => (string)el.Value).ToList();

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
                            case "unitScale":
                                UnitScale = float.Parse(e.Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                break;
                            case "materials":
                                {
                                    var parsed = new ObservableList<Observable<FileId>>();
                                    bool ignoreFirst = false;
                                    foreach (var uid in (e.Value as YamlNode).AllNodes)
                                    {
                                        if (!ignoreFirst)
                                        {
                                            ignoreFirst = true;
                                            continue;
                                        }

                                        parsed.Add((FileId)uid.ToString());
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