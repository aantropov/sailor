using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Utility;
using SailorEngine;
using System.ComponentModel;
using YamlDotNet.Core;
using YamlDotNet.Serialization;
using Scalar = YamlDotNet.Core.Events.Scalar;

namespace SailorEditor.ViewModels;

public partial class Uniform<T> : ObservableObject, ICloneable
where T : IComparable<T>
{
    public object Clone()
    {
        var res = new Uniform<T> { Key = Key };

        if (Value is ICloneable cloneable)
        {
            res.Value = (T)cloneable.Clone();
        }
        else
        {
            res.Value = Value;
        }

        return res;
    }

    public override bool Equals(object obj) => obj is Uniform<T> other && Key.CompareTo(other.Key) == 0;

    public override string ToString() => $"{Key}: {Value}";

    public override int GetHashCode() => Key?.GetHashCode() ?? 0;

    private void Value_PropertyChanged(object sender, PropertyChangedEventArgs e) => OnPropertyChanged(nameof(Value));

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
                    {
                        propChanged.PropertyChanged -= Value_PropertyChanged;
                    }
                }

                SetProperty(ref _value, value, nameof(Value));

                if (_value != null)
                {
                    if (_value is INotifyPropertyChanged propChanged)
                    {
                        propChanged.PropertyChanged += Value_PropertyChanged;
                    }
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
        {
            valueConverters = [.. ValueConverters];
        }
    }

    public bool Accepts(Type type) => type == typeof(Uniform<T>);

    public object ReadYaml(IParser parser, Type type)
    {
        var deserializerBuilder = SerializationUtils.CreateDeserializerBuilder();

        foreach (var valueConverter in valueConverters)
        {
            deserializerBuilder.WithTypeConverter(valueConverter);
        }

        var deserializer = deserializerBuilder.Build();

        var key = parser.Consume<Scalar>().Value;
        var value = deserializer.Deserialize<T>(parser);
        var uniform = new Uniform<T> { Key = key, Value = value };

        return uniform;
    }

    public void WriteYaml(IEmitter emitter, object value, Type type)
    {
        var uniform = (Uniform<T>)value;
        var serializerBuilder = SerializationUtils.CreateSerializerBuilder();

        foreach (var valueConverter in valueConverters)
        {
            serializerBuilder.WithTypeConverter(valueConverter);
        }

        var serializer = serializerBuilder.Build();

        emitter.Emit(new YamlDotNet.Core.Events.Scalar(null, uniform.Key));
        serializer.Serialize(emitter, uniform.Value);
    }

    List<IYamlTypeConverter> valueConverters = [];
}

public sealed class UniformFloat : Uniform<float> { }
public sealed class UniformVec4 : Uniform<Vec4> { }
public sealed class UniformFileId : Uniform<FileId> { }
