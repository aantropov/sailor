using SailorEditor.Engine;
using SailorEditor.Helpers;
using System.ComponentModel;
using WinRT;
using YamlDotNet.RepresentationModel;

namespace SailorEditor.ViewModels
{
    public class AssetFile : INotifyPropertyChanged
    {
        public FileInfo Asset { get; set; }
        public FileInfo AssetInfo { get; set; }
        public Dictionary<string, string> Properties { get; set; } = new();

        public string Name { get; set; }
        public int Id { get; set; }
        public int FolderId { get; set; }
        public bool IsDirty { get; protected set; } = false;

        public event PropertyChangedEventHandler PropertyChanged;
        public virtual bool PreloadResources() { return true; }
        protected bool IsLoaded { get; set; }
    }
    public class TextureFile : AssetFile
    {
        public ImageSource Thumbnail { get; set; }
        public bool ShouldGenerateMips { get; set; }
        public bool ShouldSupportStorageBinding { get; set; }
        public TextureClamping Clamping { get; set; }
        public TextureFiltration Filtration { get; set; }

        public TextureFormat Format { get; set; }

        public override bool PreloadResources()
        {
            if (!IsLoaded)
            {
                try
                {
                    Thumbnail = TemplateBuilder.ResizeImageToThumbnail(Asset.FullName);
                }
                catch (Exception ex)
                {
                    Name = ex.Message;
                }

                try
                {
                    foreach (var e in Properties)
                    {
                        switch (e.Key.ToString())
                        {
                            case "bShouldGenerateMips":
                                ShouldGenerateMips = bool.Parse(e.Value);
                                break;
                            case "bShouldSupportStorageBinding":
                                ShouldSupportStorageBinding = bool.Parse(e.Value);
                                break;
                            case "clamping":
                                {
                                    TextureClamping outEnum;
                                    if (Enum.TryParse(e.Value, out outEnum))
                                        Clamping = outEnum;
                                    else
                                        return false;
                                    break;
                                }
                            case "filtration":
                                {
                                    TextureFiltration outEnum;
                                    if (Enum.TryParse(e.Value, out outEnum))
                                        Filtration = outEnum;
                                    else
                                        return false;
                                    break;
                                }
                            case "format":
                                {
                                    TextureFormat outEnum;
                                    if (Enum.TryParse(e.Value, out outEnum))
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
                    Name = e.Message;
                    return false;
                }

                IsLoaded = true;
            }

            return true;
        }
    }

    public class ShaderLibraryFile : AssetFile
    {
        public string Code { get; set; }

        public override bool PreloadResources()
        {
            if (IsLoaded)
                return true;

            Code = File.ReadAllText(Asset.FullName);

            IsLoaded = true;
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
                Name = e.ToString();
                return false;
            }

            IsLoaded = true;

            return true;
        }
    }
}