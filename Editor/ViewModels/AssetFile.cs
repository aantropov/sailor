using YamlDotNet.RepresentationModel;

namespace SailorEditor.ViewModels
{
    public class AssetFile
    {
        public FileInfo Asset { get; set; }
        public FileInfo AssetInfo { get; set; }
        public Dictionary<string, string> Properties { get; set; } = new();

        public string Name { get; set; }
        public int Id { get; set; }
        public int FolderId { get; set; }


        public virtual bool PreloadResources() { return true; }
        protected bool IsLoaded { get; set; }
    }

    public class TextureFile : AssetFile
    {
        public ImageSource ImageSource { get; set; }
        public override bool PreloadResources()
        {
            if (!IsLoaded)
            {
                ImageSource = ImageSource.FromFile(Asset.FullName);

                IsLoaded = true;
            }

            return true;
        }
    }

    public class ShaderFile : AssetFile
    {
        public string Includes { get; set; }
        public string Defines { get; set; }
        public string GlslCommonShader { get; set; }
        public string GlslVertexShader { get; set; }
        public string GlslFragmentShader { get; set; }
        public string GlslComputeShader { get; set; }

        public override bool PreloadResources()
        {
            if (IsLoaded)
                return true;

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

            IsLoaded = true;

            return true;
        }
    }
}