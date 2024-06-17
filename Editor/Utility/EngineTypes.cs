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

namespace SailorEngine
{
    public class PropertyBase { public string Typename { get; set; } }

    public class Property<T> : PropertyBase
    {
        public Property() { Typename = typeof(T).Name; }
        public T DefaultValue { get; set; } = default;
    };

    public class FloatProperty : Property<float> { }
    public class Vec4Property : Property<SailorEditor.Vec4> { }
    public class Vec3Property : Property<SailorEditor.Vec3> { }
    public class Vec2Property : Property<SailorEditor.Vec2> { }
    public class FileIdProperty : Property<FileId> { }
    public class InstanceIdProperty : Property<string> { }
    public class ObjectPtrProperty : PropertyBase { }
    public class EnumProperty : Property<string> { }
    public partial class ObjectPtr : ObservableObject, ICloneable, IComparable<ObjectPtr>
    {
        [ObservableProperty]
        FileId fileId;

        [ObservableProperty]
        string instanceId;

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

    public class FileId : IComparable<FileId>, IComparable<string>, ICloneable
    {
        public FileId() { }
        public FileId(string v) { Value = v; }

        public string Value = "";

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

            var rootNode = deserializer.Deserialize<RootNode>(yamlContent);

            var res = new EngineTypes();

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

                newComponent.Properties["fileId"] = new FileIdProperty() { DefaultValue = "NullFileId" };
                newComponent.Properties["instanceId"] = new InstanceIdProperty();

                res.Components[component.Typename] = newComponent;
            }

            foreach (var enumNode in rootNode.Enums)
            {
                foreach (var enumEntry in enumNode)
                {
                    res.Enums[enumEntry.Key] = enumEntry.Value;
                }
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
            public Dictionary<string, string> DefaultValues { get; set; }
        }
    };
}

namespace SailorEditor
{
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
};