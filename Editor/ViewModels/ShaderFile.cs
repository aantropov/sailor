using YamlDotNet.RepresentationModel;

namespace SailorEditor.ViewModels
{
    public class ShaderFile : AssetFile
    {
        public string Includes { get; set; }
        public string Defines { get; set; }
        public string GlslCommonShader { get; set; }
        public string GlslVertexShader { get; set; }
        public string GlslFragmentShader { get; set; }
        public string GlslComputeShader { get; set; }

        public override async Task<bool> PreloadResources(bool force)
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
                                Includes += e.Value.ToString();
                                break;
                            case "defines":
                                Defines += e.Value.ToString();
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