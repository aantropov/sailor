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
    public class ShaderFile : AssetFile
    {
        public string Includes { get; set; }
        public string Defines { get; set; }
        public string GlslCommonShader { get; set; }
        public string GlslVertexShader { get; set; }
        public string GlslFragmentShader { get; set; }
        public string GlslComputeShader { get; set; }

        public override bool PreloadResources(bool force)
        {
            if (IsLoaded && !force)
                return true;

            try
            {
                using (var yamlAssetInfo = new FileStream(Asset.FullName, FileMode.Open))
                using (var reader = new StreamReader(yamlAssetInfo))
                {
                    var yaml = new YamlStream();
                    yaml.Load(reader);

                    var root = (YamlMappingNode)yaml.Documents[0].RootNode;

                    foreach (var e in root.Children)
                    {
                        switch (e.Key.ToString())
                        {
                            case "includes":
                                Includes += e.Value.ToString() + "\n";
                                break;
                            case "defines":
                                Defines += e.Value.ToString() + "\n";
                                break;
                            case "glslVertex":
                                GlslVertexShader = e.Value.ToString();
                                break;
                            case "glslFragment":
                                GlslFragmentShader = e.Value.ToString();
                                break;
                            case "glslCommon":
                                GlslCommonShader = e.Value.ToString();
                                break;
                            case "glslCompute":
                                GlslComputeShader = e.Value.ToString();
                                break;
                        }
                    }

                    yamlAssetInfo.Close();
                }
            }
            catch (Exception e)
            {
                DisplayName = e.ToString();
                return false;
            }

            IsLoaded = true;

            return true;
        }
    }
}