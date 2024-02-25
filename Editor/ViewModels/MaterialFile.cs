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
using CommunityToolkit.Mvvm.ComponentModel;
using System.Collections.ObjectModel;
using CommunityToolkit.Maui.Core.Extensions;
using SailorEditor.Utility;
using System.Collections.Generic;

namespace SailorEditor.ViewModels
{
    using AssetUID = string;
    using BlendMode = Engine.BlendMode;
    public partial class MaterialFile : AssetFile
    {
        [ObservableProperty]
        private string renderQueue = "Opaque";

        [ObservableProperty]
        private float depthBias = 0.0f;

        [ObservableProperty]
        private bool supportMultisampling = true;

        [ObservableProperty]
        private bool customDepthShader = true;

        [ObservableProperty]
        private bool enableDepthTest = true;

        [ObservableProperty]
        private bool enableZWrite = true;

        [ObservableProperty]
        private CullMode cullMode;

        [ObservableProperty]
        private BlendMode blendMode;

        [ObservableProperty]
        private FillMode fillMode;

        [ObservableProperty]
        private AssetUID shader;

        [ObservableProperty]
        private Dictionary<string, AssetUID> samplers = new();

        [ObservableProperty]
        private Dictionary<string, Vector4> uniformsVec4 = new();

        [ObservableProperty]
        private Dictionary<string, float> uniformsFloat = new();

        [ObservableProperty]
        private TrulyObservableCollection<ObservableString> shaderDefines = new();
        private Dictionary<string, object> AssetProperties { get; set; } = new();

        protected override void UpdateModel()
        {
            AssetProperties["bEnableDepthTest"] = EnableDepthTest.ToString();
            AssetProperties["bEnableZWrite"] = EnableZWrite.ToString();
            AssetProperties["bSupportMultisampling"] = SupportMultisampling.ToString();
            AssetProperties["bCustomDepthShader"] = CustomDepthShader.ToString();
            AssetProperties["depthBias"] = DepthBias.ToString();
            AssetProperties["renderQueue"] = RenderQueue.ToString();
            AssetProperties["cullMode"] = CullMode.ToString();
            AssetProperties["blendMode"] = BlendMode.ToString();
            AssetProperties["fillMode"] = FillMode.ToString();
            AssetProperties["shaderUid"] = Shader.ToString();
            
            // Collections
            AssetProperties["defines"] = ShaderDefines.Select((el) => el.Str).ToList();
            AssetProperties["samplers"] = Samplers;

            var vec4 = new List<Dictionary<string, List<float>>>();
            foreach (var el in UniformsVec4)
            {
                var values = new List<float> { el.Value.X, el.Value.Y, el.Value.Z, el.Value.W };
                vec4.Add(new Dictionary<string, List<float>> { { el.Key.ToString(), values } });
            }
            AssetProperties["uniformsVec4"] = vec4;

            var floatUniforms = new List<Dictionary<string, float>>();
            foreach (var el in UniformsFloat)
            {
                floatUniforms.Add(new Dictionary<string, float> { { el.Key.ToString(), el.Value } });
            }
            AssetProperties["uniformsFloat"] = floatUniforms;

            IsDirty = false;
        }
        public override void UpdateAssetFile()
        {
            UpdateModel();

            using (var yamlAssetInfo = new FileStream(Asset.FullName, FileMode.Create))
            using (var writer = new StreamWriter(yamlAssetInfo))
            {
                var serializer = new SerializerBuilder()
                    .WithNamingConvention(CamelCaseNamingConvention.Instance)
                    .Build();

                var yaml = serializer.Serialize(AssetProperties);
                writer.Write(yaml);
            }

            IsDirty = false;
        }

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

                                    ShaderDefines = new TrulyObservableCollection<ObservableString>(parsed.Select((el) => new ObservableString(el)));
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

                    ShaderDefines.CollectionChanged += (a, e) => MarkDirty(nameof(ShaderDefines));
                    ShaderDefines.ItemChanged += (a, e) => MarkDirty(nameof(ShaderDefines));
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