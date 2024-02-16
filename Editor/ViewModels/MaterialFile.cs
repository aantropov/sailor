using Microsoft.Maui.Controls.Compatibility;
using SailorEditor.Engine;
using SailorEditor.Helpers;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Services;
using Microsoft.Maui.Graphics;
using SkiaSharp;
using System.IO;
using System.Numerics;
using System.Collections.Specialized;
using YamlDotNet.Core.Tokens;
using System.Globalization;

namespace SailorEditor.ViewModels
{
    using AssetUID = string;
    using BlendMode = Engine.BlendMode;
    public class MaterialFile : AssetFile
    {
        public string RenderQueue
        {
            get { return renderQueue; }
            set { if (renderQueue != value) { renderQueue = value; MakeDirty(nameof(RenderQueue)); } }
        }

        public float DepthBias
        {
            get { return depthBias; }
            set { if (depthBias != value) { depthBias = value; MakeDirty(nameof(DepthBias)); } }
        }

        public bool EnableDepthTest
        {
            get { return enableDepthTest; }
            set { if (enableDepthTest != value) { enableDepthTest = value; MakeDirty(nameof(EnableDepthTest)); } }
        }
        public bool EnableZWrite
        {
            get { return enableZWrite; }
            set { if (enableZWrite != value) { enableZWrite = value; MakeDirty(nameof(EnableZWrite)); } }
        }
        public bool CustomDepthShader
        {
            get { return customDepthShader; }
            set { if (customDepthShader != value) { customDepthShader = value; MakeDirty(nameof(CustomDepthShader)); } }
        }
        public bool SupportMultisampling
        {
            get { return supportMultisampling; }
            set { if (supportMultisampling != value) { supportMultisampling = value; MakeDirty(nameof(SupportMultisampling)); } }
        }
        public Dictionary<string, float> UniformsFloat
        {
            get { return uniformsFloat; }
            set { if (uniformsFloat != value) { uniformsFloat = value; MakeDirty(nameof(UniformsFloat)); } }
        }
        public Dictionary<string, Vector4> UniformsVec4
        {
            get { return uniformsVec4; }
            set { if (uniformsVec4 != value) { uniformsVec4 = value; MakeDirty(nameof(UniformsVec4)); } }
        }
        public Dictionary<string, AssetUID> Samplers
        {
            get { return samplers; }
            set { if (samplers != value) { samplers = value; MakeDirty(nameof(Samplers)); } }
        }
        public AssetUID Shader
        {
            get { return shader; }
            set { if (shader != value) { shader = value; MakeDirty(nameof(Shader)); } }
        }

        public FillMode FillMode
        {
            get { return fillMode; }
            set { if (fillMode != value) { fillMode = value; MakeDirty(nameof(FillMode)); } }
        }

        public BlendMode BlendMode
        {
            get { return blendMode; }
            set { if (blendMode != value) { blendMode = value; MakeDirty(nameof(BlendMode)); } }
        }

        public CullMode CullMode
        {
            get { return cullMode; }
            set { if (cullMode != value) { cullMode = value; MakeDirty(nameof(CullMode)); } }
        }
        public List<string> Defines
        {
            get { return defines; }
            set { if (defines != value) { defines = value; MakeDirty(nameof(Defines)); } }
        }

        private string renderQueue = "Opaque";
        private float depthBias = 0.0f;
        private bool supportMultisampling = true;
        private bool customDepthShader = true;
        private bool enableDepthTest = true;
        private bool enableZWrite = true;
        private CullMode cullMode;
        private BlendMode blendMode;
        private FillMode fillMode;
        private AssetUID shader;
        private Dictionary<string, AssetUID> samplers = new();
        private Dictionary<string, Vector4> uniformsVec4 = new();
        private Dictionary<string, float> uniformsFloat = new();
        private List<string> defines = new();
        private Dictionary<string, object> AssetProperties { get; set; } = new();
        protected override void UpdateModel() { }
        public override bool PreloadResources(bool force)
        {
            if (!IsLoaded || force)
            {
                try
                {
                    AssetProperties = AssetsService.ParseYaml(Asset.FullName);

                    foreach (var e in AssetProperties)
                    {
                        switch (e.Key.ToString())
                        {
                            case "bEnableDepthTest":
                                EnableDepthTest = bool.Parse(e.Value.ToString());
                                break;
                            case "bEnableZWrite":
                                EnableZWrite = bool.Parse(e.Value.ToString());
                                break;
                            case "bSupportMultisampling":
                                SupportMultisampling = bool.Parse(e.Value.ToString());
                                break;
                            case "bCustomDepthShader":
                                CustomDepthShader = bool.Parse(e.Value.ToString());
                                break;
                            case "depthBias":
                                DepthBias = float.Parse(e.Value.ToString());
                                break;
                            case "renderQueue":
                                RenderQueue = e.Value.ToString();
                                break;
                            case "defines":
                                {
                                    var parsed = new List<string>();
                                    bool ignoreFirst = false;
                                    foreach (var uid in (e.Value as YamlNode).AllNodes)
                                    {
                                        if (!ignoreFirst)
                                        {
                                            ignoreFirst = true;
                                            continue;
                                        }

                                        parsed.Add(uid.ToString());
                                    }

                                    Defines = parsed;
                                }
                                break;
                            case "cullMode":
                                {
                                    CullMode outEnum;
                                    if (Enum.TryParse(e.Value.ToString(), out outEnum))
                                        CullMode = outEnum;
                                    else
                                        return false;
                                }
                                break;
                            case "blendMode":
                                {
                                    BlendMode outEnum;
                                    if (Enum.TryParse(e.Value.ToString(), out outEnum))
                                        BlendMode = outEnum;
                                    else
                                        return false;
                                }
                                break;
                            case "fillMode":
                                {
                                    FillMode outEnum;
                                    if (Enum.TryParse(e.Value.ToString(), out outEnum))
                                        FillMode = outEnum;
                                    else
                                        return false;
                                }
                                break;
                            case "shaderUid":
                                Shader = e.Value.ToString();
                                break;
                            case "samplers":
                                {
                                    var parsed = new Dictionary<string, AssetUID>();
                                    foreach (var el in (e.Value as dynamic).Children)
                                        foreach (var kv in el.Children)
                                            parsed[kv.Key.ToString()] = kv.Value.ToString();

                                    Samplers = parsed;
                                }
                                break;
                            case "uniformsVec4":
                                {
                                    var parsed = new Dictionary<string, Vector4>();
                                    foreach (var el in (e.Value as dynamic).Children)
                                        foreach (var kv in el.Children)
                                        {
                                            var vec4 = kv.Value.Children;

                                            var v = new Vector4();
                                            v.X = float.Parse(vec4[0].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                            v.Y = float.Parse(vec4[1].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                            v.Z = float.Parse(vec4[2].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                            v.W = float.Parse(vec4[3].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);

                                            parsed[kv.Key.ToString()] = v;
                                        }

                                    UniformsVec4 = parsed;
                                }
                                break;
                            case "uniformsFloat":
                                {
                                    var parsed = new Dictionary<string, float>();
                                    foreach (var el in (e.Value as dynamic).Children)
                                        foreach (var kv in el.Children)
                                            parsed[kv.Key.ToString()] = float.Parse(kv.Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);

                                    UniformsFloat = parsed;
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