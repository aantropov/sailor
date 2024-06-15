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
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using YamlDotNet.Core.Tokens;

namespace SailorEditor.ViewModels
{
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

    public class UniformYamlConverter<T> : IYamlTypeConverter where T : IComparable<T>
    {
        public UniformYamlConverter(IYamlTypeConverter[] ValueConverters = null)
        {
            if (ValueConverters != null)
                valueConverters = [.. ValueConverters];
        }

        public bool Accepts(Type type) => type == typeof(Uniform<T>);

        public object ReadYaml(IParser parser, Type type)
        {
            var deserializerBuilder = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .IgnoreUnmatchedProperties()
                .IncludeNonPublicProperties();

            foreach (var valueConverter in valueConverters)
            {
                deserializerBuilder.WithTypeConverter(valueConverter);
            }

            var deserializer = deserializerBuilder.Build();

            var key = parser.Consume<YamlDotNet.Core.Events.Scalar>().Value;
            var value = deserializer.Deserialize<T>(parser);
            var uniform = new Uniform<T> { Key = key, Value = value };

            return uniform;
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var uniform = (Uniform<T>)value;
            var serializerBuilder = new SerializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance);

            foreach (var valueConverter in valueConverters)
            {
                serializerBuilder.WithTypeConverter(valueConverter);
            }

            var serializer = serializerBuilder.Build();

            emitter.Emit(new YamlDotNet.Core.Events.Scalar(null, uniform.Key));
            serializer.Serialize(emitter, uniform.Value);
        }

        List<IYamlTypeConverter> valueConverters = new();
    }

    //TODO: Implement Yaml serialization
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
        private string cullMode;

        [ObservableProperty]
        private string blendMode;

        [ObservableProperty]
        private string fillMode;

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

        public override async Task<bool> LoadDependentResources()
        {
            if (!IsLoaded)
            {
                try
                {
                    IsDirty = false;

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