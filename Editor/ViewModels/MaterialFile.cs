using System.ComponentModel;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using SailorEditor.Services;
using System.Numerics;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEngine;
using SailorEditor.Utility;

namespace SailorEditor.ViewModels
{
    using BlendMode = SailorEngine.BlendMode;

    public partial class Uniform<T> : ObservableObject, ICloneable
    where T : IComparable<T>
    {
        public object Clone()
        {
            var res = new Uniform<T> { Key = Key };

            if (Value is ICloneable cloneable)
                res.Value = (T)cloneable.Clone();
            else
                res.Value = Value;

            return res;
        }

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

        private void Value_PropertyChanged(object sender, PropertyChangedEventArgs e)
        {
            OnPropertyChanged(nameof(Value));
        }

        public T Value
        {
            get => _value;
            set
            {
                if (!Equals(_value, value))
                {
                    if (_value != null)
                    {
                        if (_value is INotifyPropertyChanged propChanged)
                            propChanged.PropertyChanged -= Value_PropertyChanged;
                    }

                    SetProperty(ref _value, value, nameof(Value));

                    if (_value != null)
                    {
                        if (_value is INotifyPropertyChanged propChanged)
                            propChanged.PropertyChanged += Value_PropertyChanged;
                    }
                }
            }
        }


        [ObservableProperty]
        string key;

        T _value;
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
        private FileId shader;

        [ObservableProperty]
        private ObservableList<Uniform<FileId>> samplers = new();

        [ObservableProperty]
        private ObservableList<Uniform<Vec4>> uniformsVec4 = new();

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
            AssetProperties["shaderUid"] = Shader.Value;

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
                                DepthBias = float.Parse(e.Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
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
                                    var parsed = new ObservableList<Uniform<FileId>>();

                                    foreach (var el in (e.Value as dynamic).Children)
                                    {
                                        parsed.Add(new Uniform<FileId> { Key = el.Key.ToString(), Value = el.Value.ToString() });
                                    }

                                    Samplers = parsed;
                                }
                                break;
                            case "uniformsVec4":
                                {
                                    var parsed = new ObservableList<Uniform<Vec4>>();
                                    foreach (var el in (e.Value as dynamic).Children)
                                    {
                                        var vec4 = el.Value.Children;

                                        var v = new Vector4();
                                        v.X = float.Parse(vec4[0].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                        v.Y = float.Parse(vec4[1].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                        v.Z = float.Parse(vec4[2].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);
                                        v.W = float.Parse(vec4[3].Value.ToString(), CultureInfo.InvariantCulture.NumberFormat);

                                        parsed.Add(new Uniform<Vec4> { Key = el.Key.ToString(), Value = v });
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

                    UniformsVec4.CollectionChanged += (a, e) => MarkDirty(nameof(UniformsVec4));
                    UniformsVec4.ItemChanged += (a, e) => MarkDirty(nameof(UniformsVec4));

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