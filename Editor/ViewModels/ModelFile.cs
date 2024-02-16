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
    using AssetUID = string;
    public class ModelFile : AssetFile
    {
        public bool ShouldGenerateMaterials
        {
            get { return shouldGenerateMaterials; }
            set
            {
                if (shouldGenerateMaterials != value) { shouldGenerateMaterials = value; MakeDirty(nameof(ShouldGenerateMaterials)); }
            }
        }
        public bool ShouldBatchByMaterial
        {
            get { return shouldBatchByMaterial; }
            set { if (shouldBatchByMaterial != value) { shouldBatchByMaterial = value; MakeDirty(nameof(ShouldBatchByMaterial)); } }
        }

        public List<AssetUID> DefaultMaterials
        {
            get { return defaultMaterials; }
            set { if (DefaultMaterials != value) { defaultMaterials = value; MakeDirty(nameof(DefaultMaterials)); } }
        }

        private bool shouldGenerateMaterials;
        private bool shouldBatchByMaterial;
        private List<AssetUID> defaultMaterials = new();

        protected override void UpdateModel()
        {
            Properties["bShouldGenerateMaterials"] = ShouldGenerateMaterials.ToString();
            Properties["bShouldBatchByMaterial"] = ShouldBatchByMaterial.ToString();
            Properties["defaultMaterials"] = defaultMaterials.ToString();

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

                                    DefaultMaterials = parsed;
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