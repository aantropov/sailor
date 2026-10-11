using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using SailorEngine;
using System.Collections;
using System.Globalization;
using YamlDotNet.Core;
using YamlDotNet.Core.Events;
using YamlDotNet.Serialization;

namespace SailorEditor.Utility;

internal sealed class ReflectedValueCodec(EngineTypes catalog)
{
    readonly ISerializer serializer = SerializationUtils.CreateSerializerBuilder().Build();
    readonly IValueSerializer valueSerializer = SerializationUtils.CreateSerializerBuilder()
        .WithQuotingNecessaryStrings().BuildValueSerializer();
    readonly IDeserializer deserializer = SerializationUtils.CreateDeserializerBuilder().Build();

    public ObservableObject CreateDefault(PropertyBase property) =>
        Read(property, property.SerializedDefault, property.Typename, "default");

    public ObservableObject Read(PropertyBase property, object value, string ownerType, string propertyName)
    {
        var scalar = Convert.ToString(value, CultureInfo.InvariantCulture) ?? string.Empty;
        return property switch
        {
            RecordProperty record => ReadRecord(record, value),
            ListProperty list => ReadList(list, value, ownerType, propertyName),
            RotationProperty rotation => value is null
                ? (Rotation)rotation.DefaultValue.Clone() : Deserialize<Rotation>(value),
            Vec2Property => value is null ? new Vec2() : Deserialize<Vec2>(value),
            Vec3Property => value is null ? new Vec3() : Deserialize<Vec3>(value),
            Vec4Property => value is null ? new Vec4() : Deserialize<Vec4>(value),
            FileIdProperty id => new Observable<FileId>(value is null
                ? (FileId)(id.DefaultValue?.Clone() ?? new FileId()) : new FileId(scalar)),
            InstanceIdProperty id => new Observable<InstanceId>(value is null
                ? (InstanceId)(id.DefaultValue?.Clone() ?? new InstanceId()) : new InstanceId(scalar)),
            Property<List<FileId>> => new ObservableFileIdList(value is null ? [] : Deserialize<List<FileId>>(value)),
            Property<List<float>> => new ObservableFloatList(value is null ? [] : Deserialize<List<float>>(value)),
            ObjectPtrProperty => value is null ? new ObjectPtr() : Deserialize<ObjectPtr>(value),
            EnumProperty enumeration => new Observable<string>(ReadEnum(
                enumeration, value, ownerType, propertyName)),
            Property<string> text => new Observable<string>(value is null ? text.DefaultValue ?? "" : scalar),
            Property<bool> boolean => new Observable<bool>(value is null ? boolean.DefaultValue
                : (bool)EditorComponentScalarCodec.Parse(EditorComponentScalarKind.Boolean, scalar)),
            Property<int> integer => new Observable<int>(value is null ? integer.DefaultValue
                : (int)EditorComponentScalarCodec.Parse(EditorComponentScalarKind.Int32, scalar)),
            Property<uint> integer => new Observable<uint>(value is null ? integer.DefaultValue
                : (uint)EditorComponentScalarCodec.Parse(EditorComponentScalarKind.UInt32, scalar)),
            FloatProperty number => new Observable<float>(value is null ? number.DefaultValue
                : (float)EditorComponentScalarCodec.Parse(EditorComponentScalarKind.Float, scalar)),
            _ => throw new InvalidOperationException($"Unsupported reflected property '{ownerType}.{propertyName}'.")
        };
    }

    ObservableRecord ReadRecord(RecordProperty property, object value)
    {
        if (value is not null && value is not IDictionary)
            throw new YamlException($"Expected a mapping for '{property.Typename}'.");

        var fields = (IDictionary)value;
        if (fields is not null)
        {
            foreach (var key in fields.Keys)
                if (key is not string name || !property.RecordType.Properties.ContainsKey(name))
                    throw new YamlException($"Unknown field '{key}' for '{property.Typename}'.");
        }

        var result = new ObservableRecord();
        foreach (var field in property.RecordType.Properties)
        {
            result.Fields.Add(field.Key, fields?.Contains(field.Key) == true
                ? Read(field.Value, fields[field.Key], property.Typename, field.Key)
                : CreateDefault(field.Value));
        }
        return result;
    }

    ObservableValueList ReadList(ListProperty property, object value, string ownerType, string propertyName)
    {
        var result = new ObservableValueList();
        if (value is null)
            return result;
        if (value is not IList items)
            throw new YamlException($"Expected a sequence for '{ownerType}.{propertyName}'.");
        for (var i = 0; i < items.Count; ++i)
            result.Values.Add(Read(property.ElementType, items[i], ownerType, $"{propertyName}[{i}]"));
        return result;
    }

    string ReadEnum(EnumProperty property, object value, string ownerType, string propertyName)
    {
        var values = catalog.Enums[property.Typename];
        var scalar = value is null
            ? property.DefaultValue ?? values[0]
            : Convert.ToString(value, CultureInfo.InvariantCulture);
        return EditorComponentPropertyContract.ValidateEnumValue(
            ownerType, propertyName, property.Typename, scalar, values);
    }

    T Deserialize<T>(object value) => deserializer.Deserialize<T>(serializer.Serialize(value));

    public static object ToPlainValue(ObservableObject value) => value switch
    {
        ObservableRecord record => record.Fields.ToDictionary(
            field => field.Key, field => ToPlainValue(field.Value), StringComparer.Ordinal),
        ObservableValueList list => list.Values.Select(ToPlainValue).ToArray(),
        ObservableFileIdList list => list.Values.Select(id => id.Value.Value).ToArray(),
        ObservableFloatList list => list.Values.Select(number => number.Value).ToArray(),
        Observable<FileId> id => id.Value.Value,
        Observable<InstanceId> id => id.Value.Value,
        Observable<string> text => text.Value,
        Observable<bool> boolean => boolean.Value,
        Observable<int> integer => integer.Value,
        Observable<uint> integer => integer.Value,
        Observable<float> number => number.Value,
        Vec2 vector => new[] { vector.X, vector.Y },
        Vec3 vector => new[] { vector.X, vector.Y, vector.Z },
        Vec4 vector => new[] { vector.X, vector.Y, vector.Z, vector.W },
        Quat rotation => new[] { rotation.X, rotation.Y, rotation.Z, rotation.W },
        Rotation rotation => ToPlainValue(rotation.Quat),
        ObjectPtr pointer => new Dictionary<string, object>(StringComparer.Ordinal)
        {
            ["fileId"] = pointer.FileId.Value,
            ["instanceId"] = pointer.InstanceId.Value
        },
        _ => throw new InvalidOperationException($"Unsupported reflected value '{value.GetType().Name}'.")
    };

    public void Write(IEmitter emitter, ObservableObject value)
    {
        switch (value)
        {
            case ObservableRecord record:
                emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));
                foreach (var field in record.Fields)
                {
                    emitter.Emit(new Scalar(field.Key));
                    Write(emitter, field.Value);
                }
                emitter.Emit(new MappingEnd());
                break;
            case ObservableValueList list:
                WriteList(emitter, list.Values);
                break;
            case ObservableFileIdList list:
                WriteList(emitter, list.Values);
                break;
            case ObservableFloatList list:
                WriteList(emitter, list.Values);
                break;
            case Observable<FileId> id:
                valueSerializer.SerializeValue(emitter, id.Value, typeof(FileId));
                break;
            case Observable<InstanceId> id:
                valueSerializer.SerializeValue(emitter, id.Value, typeof(InstanceId));
                break;
            case Observable<string> text:
                valueSerializer.SerializeValue(emitter, text.Value, typeof(string));
                break;
            case Observable<bool> boolean:
                emitter.Emit(new Scalar(EditorComponentScalarCodec.Format(EditorComponentScalarKind.Boolean, boolean.Value)));
                break;
            case Observable<int> integer:
                emitter.Emit(new Scalar(EditorComponentScalarCodec.Format(EditorComponentScalarKind.Int32, integer.Value)));
                break;
            case Observable<uint> integer:
                emitter.Emit(new Scalar(EditorComponentScalarCodec.Format(EditorComponentScalarKind.UInt32, integer.Value)));
                break;
            case Observable<float> number:
                emitter.Emit(new Scalar(EditorComponentScalarCodec.Format(EditorComponentScalarKind.Float, number.Value)));
                break;
            case Rotation or Quat or Vec2 or Vec3 or Vec4 or ObjectPtr:
                valueSerializer.SerializeValue(emitter, value, value.GetType());
                break;
            default:
                throw new InvalidOperationException($"Unsupported reflected value '{value.GetType().Name}'.");
        }
    }

    void WriteList(IEmitter emitter, IEnumerable<ObservableObject> values)
    {
        emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
        foreach (var value in values)
            Write(emitter, value);
        emitter.Emit(new SequenceEnd());
    }
}
