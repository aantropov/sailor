using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using System.Numerics;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization;
using SailorEditor.Services;
using SailorEditor;
using System.Globalization;
using YamlDotNet.Serialization.NamingConventions;
using SailorEngine;
using System.Xml.Linq;

namespace SailorEngine
{
    public class PropertyBase { public string Typename { get; set; } }

    public class Property<T> : PropertyBase
    {
        public Property() { Typename = typeof(T).Name; }
        public T DefaultValue { get; set; } = default;
    };

    public class FloatProperty : Property<float> { }
    public class RotationProperty : Property<SailorEditor.Rotation>
    {
        public RotationProperty(Quat defaultValue) => DefaultValue = new Rotation(defaultValue);
    }

    public class Vec4Property : Property<SailorEditor.Vec4> { }
    public class Vec3Property : Property<SailorEditor.Vec3> { }
    public class Vec2Property : Property<SailorEditor.Vec2> { }
    public class FileIdProperty : Property<FileId> { }
    public class InstanceIdProperty : Property<InstanceId> { }
    public class ObjectPtrProperty : PropertyBase { }
    public class EnumProperty : Property<string> { }
    public partial class ObjectPtr : ObservableObject, ICloneable, IComparable<ObjectPtr>
    {
        [ObservableProperty]
        FileId fileId;

        [ObservableProperty]
        InstanceId instanceId;

        public object Clone() => new ObjectPtr() { FileId = FileId, InstanceId = InstanceId };

        public int CompareTo(ObjectPtr other)
        {
            if (other == null) return 1;

            int fileIdComparison = FileId.CompareTo(other.FileId);
            if (fileIdComparison != 0)
            {
                return fileIdComparison;
            }

            return string.Compare(InstanceId, other.InstanceId, StringComparison.Ordinal);
        }
    }

    public class InstanceId : IComparable<InstanceId>, IComparable<string>, ICloneable
    {
        public const string NullInstanceId = "NullInstanceId";

        public InstanceId() { }
        public InstanceId(string v) { Value = v; }

        public string Value = NullInstanceId;

        public bool IsEmpty() => Value == NullInstanceId || Value == "";

        public object Clone() => new InstanceId() { Value = Value };
        public int CompareTo(InstanceId other) => Value.CompareTo(other.Value);
        public int CompareTo(string other) => Value.CompareTo(other);

        public override bool Equals(object obj)
        {
            if (obj is InstanceId other)
            {
                return Value.CompareTo(other.Value) == 0;
            }
            else if (obj is string str)
            {
                return Value.CompareTo(str) == 0;
            }

            return false;
        }

        public override int GetHashCode() => Value?.GetHashCode() ?? 0;

        public static implicit operator string(InstanceId ts) => ts?.Value;

        public static implicit operator InstanceId(string val) => new() { Value = val };
    }

    public class FileId : IComparable<FileId>, IComparable<string>, ICloneable
    {
        public const string NullFileId = "NullFileId";

        public FileId() { }
        public FileId(string v) { Value = v; }

        public string Value = NullFileId;

        public bool IsEmpty() => Value == NullFileId || Value == "";

        public object Clone() => new FileId() { Value = Value };
        public int CompareTo(FileId other) => Value.CompareTo(other.Value);
        public int CompareTo(string other) => Value.CompareTo(other);

        public override bool Equals(object obj)
        {
            if (obj is FileId other)
            {
                return Value.CompareTo(other.Value) == 0;
            }
            else if (obj is string str)
            {
                return Value.CompareTo(str) == 0;
            }

            return false;
        }

        public override int GetHashCode() => Value?.GetHashCode() ?? 0;

        public static implicit operator string(FileId ts) => ts?.Value;

        public static implicit operator FileId(string val) => new() { Value = val };
    }

    public class ComponentType
    {
        public string Name { get; set; }
        public Dictionary<string, PropertyBase> Properties { get; set; } = [];
    };

    class EngineTypes
    {
        public Dictionary<string, ComponentType> Components { get; private set; } = [];
        public Dictionary<string, List<string>> Enums { get; private set; } = [];

        public static EngineTypes FromYaml(string yamlContent)
        {
            var deserializer = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .IgnoreUnmatchedProperties()
                .Build();

            var res = new EngineTypes();

            try
            {
                var rootNode = deserializer.Deserialize<RootNode>(yamlContent);

                foreach (var component in rootNode.EngineTypes)
                {
                    var newComponent = new ComponentType
                    {
                        Name = component.Typename
                    };

                    foreach (var property in component.Properties)
                    {
                        PropertyBase newProperty = property.Value switch
                        {
                            "struct glm::qua<float,0>" => new RotationProperty(rootNode.Cdos.Find((a) => a.Typename == component.Typename).DefaultValues[property.Key] as Quat ?? default),
                            "struct glm::vec<2,float,0>" => new Vec2Property(),
                            "struct glm::vec<3,float,0>" => new Vec3Property(),
                            "struct glm::vec<4,float,0>" => new Vec4Property(),
                            "float" => new FloatProperty(),
                            var value when value.StartsWith("class Sailor::TObjectPtr") => new ObjectPtrProperty(),
                            var value when value.Contains("TObjectPtr") => new InstanceIdProperty(),
                            var value when value.StartsWith("enum") => new EnumProperty() { Typename = value },
                            _ => throw new InvalidOperationException($"Unexpected property type: {property.Value}")
                        };


                        newComponent.Properties[property.Key] = newProperty;
                    }

                    newComponent.Properties["fileId"] = new FileIdProperty() { DefaultValue = FileId.NullFileId };
                    newComponent.Properties["instanceId"] = new InstanceIdProperty() { DefaultValue = InstanceId.NullInstanceId };

                    res.Components[component.Typename] = newComponent;
                }

                foreach (var enumNode in rootNode.Enums)
                {
                    foreach (var enumEntry in enumNode)
                    {
                        res.Enums[enumEntry.Key] = enumEntry.Value;
                    }
                }

            }
            catch (Exception ex)
            {
                Console.WriteLine(ex.Message);
            }
            return res;
        }

        public class RootNode
        {
            public List<EngineTypeNode> EngineTypes { get; set; }
            public List<ComponentDefaultValuesNode> Cdos { get; set; }
            public List<Dictionary<string, List<string>>> Enums { get; set; }
        }

        public class EngineTypeNode
        {
            public string Typename { get; set; }
            public Dictionary<string, string> Properties { get; set; }
        }

        public class ComponentDefaultValuesNode
        {
            public string Typename { get; set; }
            public Dictionary<string, object> DefaultValues { get; set; }
        }
    };
}

namespace SailorEditor
{
    public partial class Rotation : ObservableObject, ICloneable, IComparable<Rotation>
    {
        public Rotation() { }
        public Rotation(Quat value) => Quat = value;

        public Quat Quat
        {
            get => quat;
            set
            {
                if (quat != value && SetProperty(ref quat, value))
                {
                    UpdateYawPitchRoll();
                    OnPropertyChanged(nameof(Yaw));
                    OnPropertyChanged(nameof(Pitch));
                    OnPropertyChanged(nameof(Roll));
                }
            }
        }

        public float Yaw
        {
            get => yaw;
            set
            {
                if (yaw != value)
                {
                    yaw = value;
                    UpdateQuat();
                    OnPropertyChanged();
                }
            }
        }

        public float Pitch
        {
            get => pitch;
            set
            {
                if (pitch != value)
                {
                    pitch = value;
                    UpdateQuat();
                    OnPropertyChanged();
                }
            }
        }

        public float Roll
        {
            get => roll;
            set
            {
                if (roll != value)
                {
                    roll = value;
                    UpdateQuat();
                    OnPropertyChanged();
                }
            }
        }

        public static implicit operator Rotation(Quat value) => new() { Quat = value };
        public static implicit operator Quat(Rotation uniform) => new(uniform.Quat);
        public object Clone() => new Rotation { Quat = Quat };

        public override string ToString() => $"<Yaw: {Yaw}, Pitch: {Pitch}, Roll: {Roll}>";

        public int CompareTo(Rotation other) => Quat.CompareTo(other.Quat);

        private void UpdateQuat() => quat = Quat.FromYawPitchRoll(yaw, pitch, roll);
        private void UpdateYawPitchRoll() => (yaw, pitch, roll) = quat.ToYawPitchRoll();

        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;

        Quat quat;
    }

    public partial class Quat : ObservableObject, ICloneable, IComparable<Quat>
    {
        public Quat() { }
        public Quat(Quaternion value)
        {
            X = value.X;
            Y = value.Y;
            Z = value.Z;
            W = value.W;
        }

        public Quat(float x, float y, float z, float w)
        {
            X = x;
            Y = y;
            Z = z;
            W = w;
        }

        public static implicit operator Quat(Quaternion value) => new() { X = value.X, Y = value.Y, Z = value.Z, W = value.W };
        public static implicit operator Quaternion(Quat uniform) => new(uniform.X, uniform.Y, uniform.Z, uniform.W);
        public object Clone() => new Quat { X = X, Y = Y, Z = Z, W = W };
        public override string ToString() => $"<{X} {Y} {Z} {W}>";

        public int CompareTo(Quat other)
        {
            if (other == null) return 1;

            int result = X.CompareTo(other.X);
            if (result != 0) return result;

            result = Y.CompareTo(other.Y);
            if (result != 0) return result;

            result = Z.CompareTo(other.Z);
            if (result != 0) return result;

            return W.CompareTo(other.W);
        }

        public (float yaw, float pitch, float roll) ToYawPitchRoll()
        {
            // Convert quaternion to yaw, pitch, roll
            float ysqr = Y * Y;

            // Roll (x-axis rotation)
            float t0 = +2.0f * (W * X + Y * Z);
            float t1 = +1.0f - 2.0f * (X * X + ysqr);
            float roll = MathF.Atan2(t0, t1);

            // Pitch (y-axis rotation)
            float t2 = +2.0f * (W * Y - Z * X);
            t2 = t2 > 1.0f ? 1.0f : t2;
            t2 = t2 < -1.0f ? -1.0f : t2;
            float pitch = MathF.Asin(t2);

            // Yaw (z-axis rotation)
            float t3 = +2.0f * (W * Z + X * Y);
            float t4 = +1.0f - 2.0f * (ysqr + Z * Z);
            float yaw = MathF.Atan2(t3, t4);

            // Convert from radians to degrees
            return (Sailor.Mathf.ToDegrees(yaw), Sailor.Mathf.ToDegrees(pitch), Sailor.Mathf.ToDegrees(roll));
        }

        public static Quat FromYawPitchRoll(float yaw, float pitch, float roll)
        {
            float yawRad = Sailor.Mathf.ToRadians(yaw);
            float pitchRad = Sailor.Mathf.ToRadians(pitch);
            float rollRad = Sailor.Mathf.ToRadians(roll);

            float cy = Sailor.Mathf.Cos(yawRad * 0.5f);
            float sy = Sailor.Mathf.Sin(yawRad * 0.5f);
            float cp = Sailor.Mathf.Cos(pitchRad * 0.5f);
            float sp = Sailor.Mathf.Sin(pitchRad * 0.5f);
            float cr = Sailor.Mathf.Cos(rollRad * 0.5f);
            float sr = Sailor.Mathf.Sin(rollRad * 0.5f);

            Quat quat = new Quat
            {
                W = cr * cp * cy + sr * sp * sy,
                X = sr * cp * cy - cr * sp * sy,
                Y = cr * sp * cy + sr * cp * sy,
                Z = cr * cp * sy - sr * sp * cy
            };

            return quat;
        }

        [ObservableProperty]
        float x = 0.0f;

        [ObservableProperty]
        float y = 0.0f;

        [ObservableProperty]
        float z = 0.0f;

        [ObservableProperty]
        float w = 0.0f;
    }

    public partial class Vec4 : ObservableObject, ICloneable, IComparable<Vec4>
    {
        public Vec4() { }
        public Vec4(Vector4 value)
        {
            X = value.X;
            Y = value.Y;
            Z = value.Z;
            W = value.W;
        }

        public static implicit operator Vec4(Vector4 value) => new() { X = value.X, Y = value.Y, Z = value.Z, W = value.W };
        public static implicit operator Vector4(Vec4 uniform) => new(uniform.X, uniform.Y, uniform.Z, uniform.W);
        public object Clone() => new Vec4 { X = X, Y = Y, Z = Z, W = W };
        public override string ToString() => $"<{X} {Y} {Z} {W}>";

        public int CompareTo(Vec4 other)
        {
            if (other == null) return 1;

            int result = X.CompareTo(other.X);
            if (result != 0) return result;

            result = Y.CompareTo(other.Y);
            if (result != 0) return result;

            result = Z.CompareTo(other.Z);
            if (result != 0) return result;

            return W.CompareTo(other.W);
        }

        [ObservableProperty]
        float x = 0.0f;

        [ObservableProperty]
        float y = 0.0f;

        [ObservableProperty]
        float z = 0.0f;

        [ObservableProperty]
        float w = 0.0f;
    }

    public partial class Vec3 : ObservableObject, ICloneable, IComparable<Vec3>
    {
        public Vec3() { }
        public Vec3(Vector3 value)
        {
            X = value.X;
            Y = value.Y;
            Z = value.Z;
        }

        public static implicit operator Vec3(Vector3 value) => new() { X = value.X, Y = value.Y, Z = value.Z };
        public static implicit operator Vector3(Vec3 uniform) => new(uniform.X, uniform.Y, uniform.Z);
        public object Clone() => new Vec3 { X = X, Y = Y, Z = Z };
        public override string ToString() => $"<{X} {Y} {Z}>";

        public int CompareTo(Vec3 other)
        {
            if (other == null)
                return 1;

            int result = X.CompareTo(other.X);
            if (result != 0)
                return result;

            result = Y.CompareTo(other.Y);
            if (result != 0)
                return result;

            result = Z.CompareTo(other.Z);
            return result;
        }

        [ObservableProperty]
        float x = 0.0f;

        [ObservableProperty]
        float y = 0.0f;

        [ObservableProperty]
        float z = 0.0f;
    }

    public partial class Vec2 : ObservableObject, ICloneable, IComparable<Vec2>
    {
        public Vec2() { }
        public Vec2(Vector3 value)
        {
            X = value.X;
            Y = value.Y;
        }

        public static implicit operator Vec2(Vector2 value) => new() { X = value.X, Y = value.Y };
        public static implicit operator Vector2(Vec2 uniform) => new(uniform.X, uniform.Y);
        public object Clone() => new Vec2 { X = X, Y = Y };
        public override string ToString() => $"<{X} {Y}>";

        public int CompareTo(Vec2 other)
        {
            if (other == null)
                return 1;

            int result = X.CompareTo(other.X);
            if (result != 0)
                return result;

            result = Y.CompareTo(other.Y);

            return result;
        }

        [ObservableProperty]
        float x = 0.0f;

        [ObservableProperty]
        float y = 0.0f;
    }

    public class Vec2YamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec2);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.Consume<SequenceStart>();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value, CultureInfo.InvariantCulture.NumberFormat));
                }
                parser.MoveNext();
            }
            parser.Consume<SequenceEnd>();
            return new Vec2 { X = list[0], Y = list[1] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Vec2)value;
            emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
            emitter.Emit(new Scalar(null, vec.X.ToString()));
            emitter.Emit(new Scalar(null, vec.Y.ToString()));
            emitter.Emit(new SequenceEnd());
        }
    }

    public class Vec3YamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec3);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.Consume<SequenceStart>();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value, CultureInfo.InvariantCulture.NumberFormat));
                }
                parser.MoveNext();
            }
            parser.Consume<SequenceEnd>();
            return new Vec3 { X = list[0], Y = list[1], Z = list[2] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Vec3)value;
            emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
            emitter.Emit(new Scalar(null, vec.X.ToString()));
            emitter.Emit(new Scalar(null, vec.Y.ToString()));
            emitter.Emit(new Scalar(null, vec.Z.ToString()));
            emitter.Emit(new SequenceEnd());
        }
    }

    public class Vec4YamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec4);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.Consume<SequenceStart>();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value, CultureInfo.InvariantCulture.NumberFormat));
                }
                parser.MoveNext();
            }
            parser.Consume<SequenceEnd>();

            return new Vec4 { X = list[0], Y = list[1], Z = list[2], W = list[3] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Vec4)value;
            emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
            emitter.Emit(new Scalar(null, vec.X.ToString()));
            emitter.Emit(new Scalar(null, vec.Y.ToString()));
            emitter.Emit(new Scalar(null, vec.Z.ToString()));
            emitter.Emit(new Scalar(null, vec.W.ToString()));
            emitter.Emit(new SequenceEnd());
        }
    }

    public class QuatYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Quat);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.Consume<SequenceStart>();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value, CultureInfo.InvariantCulture.NumberFormat));
                }
                parser.MoveNext();
            }
            parser.Consume<SequenceEnd>();

            return new Quat { X = list[0], Y = list[1], Z = list[2], W = list[3] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Quat)value;
            emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
            emitter.Emit(new Scalar(null, vec.X.ToString()));
            emitter.Emit(new Scalar(null, vec.Y.ToString()));
            emitter.Emit(new Scalar(null, vec.Z.ToString()));
            emitter.Emit(new Scalar(null, vec.W.ToString()));
            emitter.Emit(new SequenceEnd());
        }
    }

    public class RotationYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Rotation);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.Consume<SequenceStart>();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value, CultureInfo.InvariantCulture.NumberFormat));
                }
                parser.MoveNext();
            }
            parser.Consume<SequenceEnd>();

            return new Rotation(new Quat { X = list[0], Y = list[1], Z = list[2], W = list[3] });
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var quat = ((Rotation)value).Quat;
            emitter.Emit(new SequenceStart(null, null, false, SequenceStyle.Block));
            emitter.Emit(new Scalar(null, quat.X.ToString()));
            emitter.Emit(new Scalar(null, quat.Y.ToString()));
            emitter.Emit(new Scalar(null, quat.Z.ToString()));
            emitter.Emit(new Scalar(null, quat.W.ToString()));
            emitter.Emit(new SequenceEnd());
        }
    }

    public class ComponentTypeYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(SailorEngine.ComponentType);

        public object ReadYaml(IParser parser, Type type)
        {
            string name = string.Empty;

            if (parser.Current is Scalar scalar)
            {
                name = scalar.Value;
            }

            parser.MoveNext();

            return MauiProgram.GetService<EngineService>().EngineTypes.Components[name];
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var component = (SailorEngine.ComponentType)value;
            emitter.Emit(new Scalar(null, component.Name));
        }
    }

    public class FileIdYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(FileId);
        public object ReadYaml(IParser parser, Type type) => new FileId(parser.Consume<Scalar>().Value);
        public void WriteYaml(IEmitter emitter, object value, Type type) => emitter.Emit(new Scalar(((FileId)value).Value));
    }

    public class InstanceIdYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(InstanceId);
        public object ReadYaml(IParser parser, Type type) => new InstanceId(parser.Consume<Scalar>().Value);
        public void WriteYaml(IEmitter emitter, object value, Type type) => emitter.Emit(new Scalar(((InstanceId)value).Value));
    }
};