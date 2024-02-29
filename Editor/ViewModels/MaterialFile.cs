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
using WinRT;
using Windows.Foundation.Collections;
using System;

namespace SailorEditor.ViewModels
{
    using AssetUID = string;
    using BlendMode = Engine.BlendMode;

    public partial class Uniform<T> : ObservableObject, ICloneable
        where T : IComparable<T>
    {
        public object Clone() => new Uniform<T> { Key = Key, Value = Value };
        public override bool Equals(object obj)
        {
            if (obj is Uniform<T> other)
            {
                return Key.CompareTo(other.Key) == 0;
            }

            return false;
        }

        public override string ToString() => $"{Key.ToString()}: {Value.ToString()}";

        public override int GetHashCode() => Key?.GetHashCode() ?? 0;

        [ObservableProperty]
        string key;

        [ObservableProperty]
        T value;
    }

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
        private ObservableList<Uniform<AssetUID>> samplers = new();

        [ObservableProperty]
        private ObservableDictionary<Observable<string>, Vector4> uniformsVec4 = new();

        [ObservableProperty]
        private ObservableList<Uniform<float>> uniformsFloat = new();

        [ObservableProperty]
        private ObservableList<Observable<string>> shaderDefines = new();
        private Dictionary<string, object> AssetProperties { get; set; } = new();
        protected override async Task UpdateModel()
        {
            AssetProperties["bEnableDepthTest"] = EnableDepthTest;
            AssetProperties["bEnableZWrite"] = EnableZWrite;
            AssetProperties["bSupportMultisampling"] = SupportMultisampling;
            AssetProperties["bCustomDepthShader"] = CustomDepthShader;
            AssetProperties["depthBias"] = DepthBias;
            AssetProperties["renderQueue"] = RenderQueue;
            AssetProperties["cullMode"] = CullMode;
            AssetProperties["blendMode"] = BlendMode;
            AssetProperties["fillMode"] = FillMode;
            AssetProperties["shaderUid"] = Shader;

            // Collections
            AssetProperties["defines"] = ShaderDefines.Select((el) => el.Value).ToList();
            AssetProperties["samplers"] = Samplers.Select((a) => new KeyValuePair<string, string>(a.Key, a.Value)).ToDictionary();

            // We store Vec4 as List
            var vec4 = new Dictionary<string, List<float>>();
            foreach (var el in UniformsVec4)
            {
                var values = new List<float> { el.Value.X, el.Value.Y, el.Value.Z, el.Value.W };
                vec4[el.Key.ToString()] = values;
            }

            AssetProperties["uniformsVec4"] = vec4;
            AssetProperties["uniformsFloat"] = UniformsFloat.Select((a) => new KeyValuePair<string, float>(a.Key, a.Value)).ToDictionary();

            IsDirty = false;
        }
        public override async Task UpdateAssetFile()
        {
            await UpdateModel();

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

        public override async Task<bool> PreloadResources(bool force)
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

                                    ShaderDefines = new ObservableList<Observable<string>>(parsed.Select((el) => new Observable<string>(el)));
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
                                    var parsed = new ObservableList<Uniform<AssetUID>>();

                                    foreach (var el in (e.Value as dynamic).Children)
                                    {
                                        parsed.Add(new Uniform<AssetUID> { Key = el.Key.ToString(), Value = el.Value.ToString() });
                                    }

                                    Samplers = parsed;
                                }
                                break;
                            case "uniformsVec4":
                                {
                                    var parsed = new ObservableDictionary<Observable<string>, Vector4>();
                                    foreach (var el in (e.Value as dynamic).Children)
                                    {
                                        var vec4 = el.Value.Children;

                                        var v = new Vector4();
                                        v.X = float.Parse(vec4[0].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                        v.Y = float.Parse(vec4[1].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                        v.Z = float.Parse(vec4[2].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                        v.W = float.Parse(vec4[3].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);

                                        parsed[new Observable<string>(el.Key.ToString())] = v;
                                    }

                                    UniformsVec4 = parsed;
                                }
                                break;
                            case "uniformsFloat":
                                {
                                    var parsed = new ObservableList<Uniform<float>>();
                                    foreach (var el in (e.Value as dynamic).Children)
                                        parsed.Add(new Uniform<float>
                                        {
                                            Key = el.Key.ToString(),
                                            Value = float.Parse(el.Value.ToString(), CultureInfo.InvariantCulture.NumberFormat)
                                        });

                                    UniformsFloat = parsed;
                                }
                                break;
                        }
                    }

                    ShaderDefines.CollectionChanged += (a, e) => MarkDirty(nameof(ShaderDefines));
                    ShaderDefines.ItemChanged += (a, e) => MarkDirty(nameof(ShaderDefines));

                    UniformsFloat.CollectionChanged += (a, e) => MarkDirty(nameof(UniformsFloat));
                    UniformsFloat.ItemChanged += (a, e) => MarkDirty(nameof(UniformsFloat));

                    Samplers.CollectionChanged += (a, e) => MarkDirty(nameof(Samplers));
                    Samplers.ItemChanged += (a, e) => MarkDirty(nameof(Samplers));
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