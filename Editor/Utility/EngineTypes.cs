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
using System.Text.RegularExpressions;
using SailorEditor.Helpers;
using SailorEditor.Utility;
using System.ComponentModel;
using System.Linq;

namespace SailorEngine
{
    public class PropertyBase : INotifyPropertyChanged
    {
        public string Typename { get; set; }

#pragma warning disable CS0067
        public event PropertyChangedEventHandler PropertyChanged;
#pragma warning restore CS0067
    }

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
    public class ObjectPtrProperty : PropertyBase
    {
        public Type GenericType { get; set; } = null;
        public string GenericTypename { get; set; } = "";
        public bool CouldBeInstantiated { get => GenericType == null; }
    }

    public class EnumProperty : Property<string> { }
    public partial class ObjectPtr : ObservableObject, ICloneable, IComparable<ObjectPtr>
    {
        [ObservableProperty]
        FileId fileId = new();

        [ObservableProperty]
        InstanceId instanceId = new();

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

        public override string ToString() => Value ?? NullInstanceId;

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
            if (this == obj)
                return true;

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

        public override string ToString() => Value ?? NullFileId;

        public static implicit operator string(FileId ts) => ts?.Value;

        public static implicit operator FileId(string val) => new() { Value = val };
    }

    public class ComponentType
    {
        public string Name { get; set; }
        public string Base { get; set; }
        public Dictionary<string, PropertyBase> Properties { get; set; } = [];
    };

    public class AssetType
    {
        public string Name { get; set; }
        public List<string> Extensions { get; set; } = [];
        public Dictionary<string, PropertyBase> Properties { get; set; } = [];
    };

    class EngineTypes
    {
        public Dictionary<string, ComponentType> Components { get; private set; } = [];
        public Dictionary<string, AssetType> AssetTypes { get; private set; } = [];
        public Dictionary<string, AssetType> AssetTypesByExtension { get; private set; } = [];
        public Dictionary<string, List<string>> Enums { get; private set; } = [];

        public static Type GetEditorType(string engineType)
        {
            Type editorType = engineType switch
            {
                "Sailor::Model" => typeof(ModelFile),
                "class Sailor::Model" => typeof(ModelFile),
                "Sailor::Texture" => typeof(TextureFile),
                "class Sailor::Texture" => typeof(TextureFile),
                "Sailor::Material" => typeof(MaterialFile),
                "class Sailor::Material" => typeof(MaterialFile),
                "Sailor::Shader" => typeof(ShaderFile),
                "class Sailor::Shader" => typeof(ShaderFile),
                _ => null
            };

            return editorType;
        }

        static string NormalizePropertyType(string engineType)
        {
            return engineType switch
            {
                "f" => "float",
                "N3glm3quaIfLNS_9qualifierE0EEE" => "struct glm::qua<float,0>",
                "N3glm3vecILi2EfLNS_9qualifierE0EEE" => "struct glm::vec<2,float,0>",
                "N3glm3vecILi3EfLNS_9qualifierE0EEE" => "struct glm::vec<3,float,0>",
                "N3glm3vecILi4EfLNS_9qualifierE0EEE" => "struct glm::vec<4,float,0>",
                _ => engineType
            };
        }

        static string GetGenericTypeName(string engineType)
        {
            var genericMatch = Regex.Match(engineType, @"<(.+?)>");
            if (genericMatch.Success)
            {
                return genericMatch.Groups[1].Value;
            }

            return engineType switch
            {
                "N6Sailor10TObjectPtrINS_5ModelEEE" => "Sailor::Model",
                "N6Sailor10TObjectPtrINS_7TextureEEE" => "Sailor::Texture",
                "N6Sailor10TObjectPtrINS_8MaterialEEE" => "Sailor::Material",
                "N6Sailor10TObjectPtrINS_6ShaderEEE" => "Sailor::Shader",
                "N6Sailor10TObjectPtrINS_9AnimationEEE" => "Sailor::Animation",
                "N6Sailor10TObjectPtrINS_21MeshRendererComponentEEE" => "Sailor::MeshRendererComponent",
                _ => ""
            };
        }

        static void AddEnumAlias(Dictionary<string, List<string>> enums, string alias, string source)
        {
            if (!enums.ContainsKey(alias) && enums.TryGetValue(source, out var values))
            {
                enums[alias] = values;
            }
        }

        static void AddEnumAliases(Dictionary<string, List<string>> enums)
        {
            AddEnumAlias(enums, "enum Sailor::EMobilityType", "N6Sailor13EMobilityTypeE");
            AddEnumAlias(enums, "enum Sailor::ELightType", "N6Sailor10ELightTypeE");
            AddEnumAlias(enums, "enum Sailor::EAnimationPlayMode", "N6Sailor18EAnimationPlayModeE");
            AddEnumAlias(enums, "enum Sailor::RHI::EFormat", "N6Sailor3RHI7EFormatE");
            AddEnumAlias(enums, "enum Sailor::RHI::ETextureFiltration", "N6Sailor3RHI18ETextureFiltrationE");
            AddEnumAlias(enums, "enum Sailor::RHI::ETextureClamping", "N6Sailor3RHI16ETextureClampingE");
            AddEnumAlias(enums, "enum Sailor::RHI::ESamplerReductionMode", "N6Sailor3RHI21ESamplerReductionModeE");
            AddEnumAlias(enums, "enum Sailor::RHI::EFillMode", "N6Sailor3RHI9EFillModeE");
            AddEnumAlias(enums, "enum Sailor::RHI::ECullMode", "N6Sailor3RHI9ECullModeE");
            AddEnumAlias(enums, "enum Sailor::RHI::EBlendMode", "N6Sailor3RHI10EBlendModeE");
            AddEnumAlias(enums, "enum Sailor::RHI::EDepthCompare", "N6Sailor3RHI13EDepthCompareE");
            AddEnumAlias(enums, "enum Sailor::RHI::EShadowType", "N6Sailor3RHI11EShadowTypeE");
        }

        public static EngineTypes FromYaml(string yamlContent)
        {
            var deserializer = SerializationUtils.CreateDeserializerBuilder().Build();

            var res = new EngineTypes();

            try
            {
                var rootNode = deserializer.Deserialize<RootNode>(yamlContent);
                if (rootNode?.EngineTypes == null)
                    return res;

                var cdoByType = new Dictionary<string, ComponentDefaultValuesNode>();
                foreach (var cdo in rootNode.Cdos ?? new List<ComponentDefaultValuesNode>())
                {
                    if (string.IsNullOrEmpty(cdo?.Typename))
                        continue;

                    // Keep first occurrence; avoid hard failure on duplicates.
                    if (!cdoByType.ContainsKey(cdo.Typename))
                        cdoByType[cdo.Typename] = cdo;
                }

                foreach (var enumNode in rootNode.Enums ?? Enumerable.Empty<Dictionary<string, List<string>>>())
                {
                    foreach (var enumEntry in enumNode)
                    {
                        res.Enums[enumEntry.Key] = enumEntry.Value;
                    }
                }
                AddEnumAliases(res.Enums);

                foreach (var assetTypeNode in rootNode.AssetTypes ?? Enumerable.Empty<AssetTypeNode>())
                {
                    if (string.IsNullOrEmpty(assetTypeNode.Typename))
                        continue;

                    var assetType = new AssetType
                    {
                        Name = assetTypeNode.Typename,
                        Extensions = assetTypeNode.Extensions ?? []
                    };

                    foreach (var property in EnumerateAssetTypeProperties(assetTypeNode.Properties))
                    {
                        var propertyType = property.Type;
                        assetType.Properties[property.Name] = CreateAssetProperty(propertyType);
                    }

                    res.AssetTypes[assetType.Name] = assetType;
                    foreach (var extension in assetType.Extensions)
                    {
                        res.AssetTypesByExtension[extension.TrimStart('.').ToLowerInvariant()] = assetType;
                    }
                }

                // Pass 1: register all component types first.
                foreach (var component in rootNode.EngineTypes)
                {
                    if (string.IsNullOrEmpty(component.Typename))
                        continue;

                    if (!res.Components.ContainsKey(component.Typename))
                    {
                        res.Components[component.Typename] = new ComponentType
                        {
                            Name = component.Typename,
                            Base = component.Base
                        };
                    }
                }

                // Pass 2: resolve properties/defaults after all types are known.
                foreach (var component in rootNode.EngineTypes)
                {
                    if (string.IsNullOrEmpty(component.Typename))
                        continue;

                    var newComponent = res.Components[component.Typename];

                    if (component.Properties == null)
                        component.Properties = new();

                    cdoByType.TryGetValue(component.Typename, out var cdoNode);

                    foreach (var property in component.Properties)
                    {
                        try
                        {
                            string propertyType = NormalizePropertyType(property.Value);
                            string genericType = GetGenericTypeName(property.Value);

                            Quat defaultQuat = default;
                            if (cdoNode != null &&
                                cdoNode.DefaultValues != null &&
                                cdoNode.DefaultValues.TryGetValue(property.Key, out var defaultQuatObj))
                            {
                                if (defaultQuatObj is Quat q)
                                {
                                    defaultQuat = q;
                                }
                                else if (defaultQuatObj is Rotation r)
                                {
                                    defaultQuat = r.Quat;
                                }
                            }

                            PropertyBase newProperty = CreateCommonProperty(propertyType) ?? propertyType switch
                            {
                                "struct glm::qua<float,0>" => new RotationProperty(defaultQuat),
                                "struct glm::vec<2,float,0>" => new Vec2Property(),
                                "struct glm::vec<3,float,0>" => new Vec3Property(),
                                "struct glm::vec<4,float,0>" => new Vec4Property(),
                                var value when value.Contains("TObjectPtr") => new ObjectPtrProperty()
                                {
                                    GenericTypename = genericType,
                                    GenericType = GetEditorType(genericType)
                                },
                                var value when value.Contains("InstanceId") => new InstanceIdProperty(),
                                var value when value.StartsWith("enum") => new EnumProperty() { Typename = value },
                                var value when res.Enums.ContainsKey(value) => new EnumProperty() { Typename = value },
                                _ => null
                            };

                            if (newProperty != null)
                            {
                                newComponent.Properties[property.Key] = newProperty;
                            }
                        }
                        catch (Exception ex)
                        {
                            Console.WriteLine($"EngineTypes: failed to import property '{property.Key}' for '{component.Typename}': {ex.Message}");
                        }
                    }

                    newComponent.Properties["fileId"] = new FileIdProperty() { DefaultValue = FileId.NullFileId };
                    newComponent.Properties["instanceId"] = new InstanceIdProperty() { DefaultValue = InstanceId.NullInstanceId };
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine(ex.Message);
            }
            return res;
        }

        static PropertyBase CreateAssetProperty(string propertyType)
        {
            return CreateCommonProperty(propertyType) ?? propertyType switch
            {
                "FileId" => new FileIdProperty(),
                "List<FileId>" => new Property<List<FileId>>(),
                var value when value.StartsWith("enum") => new EnumProperty() { Typename = value },
                _ => throw new InvalidDataException($"Unsupported asset property type: {propertyType}")
            };
        }

        static PropertyBase CreateCommonProperty(string propertyType)
        {
            return propertyType switch
            {
                "string" => new Property<string>(),
                "bool" => new Property<bool>(),
                "int32" => new Property<int>(),
                "float" => new FloatProperty(),
                _ => null
            };
        }

        public class RootNode
        {
            public List<EngineTypeNode> EngineTypes { get; set; }
            public List<ComponentDefaultValuesNode> Cdos { get; set; }
            public List<Dictionary<string, List<string>>> Enums { get; set; }
            public List<AssetTypeNode> AssetTypes { get; set; }
        }

        public class EngineTypeNode
        {
            public string Typename { get; set; }
            public string Base { get; set; }
            public Dictionary<string, string> Properties { get; set; }
        }

        public class ComponentDefaultValuesNode
        {
            public string Typename { get; set; }
            public Dictionary<string, object> DefaultValues { get; set; }
        }

        public class AssetTypeNode
        {
            public string Typename { get; set; }
            public List<string> Extensions { get; set; }
            public object Properties { get; set; }
        }

        public class AssetTypePropertyNode
        {
            public string Name { get; set; }
            public string Type { get; set; }
        }

        static IEnumerable<AssetTypePropertyNode> EnumerateAssetTypeProperties(object properties)
        {
            if (properties is not IEnumerable<object> propertyList)
            {
                yield break;
            }

            foreach (var property in propertyList)
            {
                if (property is Dictionary<object, object> propertyNode)
                {
                    var name = propertyNode.TryGetValue("name", out var nameValue) ? nameValue?.ToString() : null;
                    var type = propertyNode.TryGetValue("type", out var typeValue) ? typeValue?.ToString() : null;
                    if (!string.IsNullOrEmpty(name) && !string.IsNullOrEmpty(type))
                    {
                        yield return new AssetTypePropertyNode { Name = name, Type = type };
                    }
                }
            }
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
            var list = YamlHelper.ParseFloatSequence(parser);
            return new Vec2 { X = list[0], Y = list[1] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Vec2)value;
            YamlHelper.EmitFloatSequence(emitter, new[] { vec.X, vec.Y });
        }
    }

    public class Vec3YamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec3);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = YamlHelper.ParseFloatSequence(parser);
            return new Vec3 { X = list[0], Y = list[1], Z = list[2] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Vec3)value;
            YamlHelper.EmitFloatSequence(emitter, new[] { vec.X, vec.Y, vec.Z });
        }
    }

    public class Vec4YamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec4);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = YamlHelper.ParseFloatSequence(parser);
            return new Vec4 { X = list[0], Y = list[1], Z = list[2], W = list[3] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Vec4)value;
            YamlHelper.EmitFloatSequence(emitter, new[] { vec.X, vec.Y, vec.Z, vec.W });
        }
    }

    public class QuatYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Quat);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = YamlHelper.ParseFloatSequence(parser);
            return new Quat { X = list[0], Y = list[1], Z = list[2], W = list[3] };
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var vec = (Quat)value;
            YamlHelper.EmitFloatSequence(emitter, new[] { vec.X, vec.Y, vec.Z, vec.W });
        }
    }

    public class RotationYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Rotation);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = YamlHelper.ParseFloatSequence(parser);
            return new Rotation(new Quat { X = list[0], Y = list[1], Z = list[2], W = list[3] });
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var quat = ((Rotation)value).Quat;
            YamlHelper.EmitFloatSequence(emitter, new[] { quat.X, quat.Y, quat.Z, quat.W });
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

    public class ObjectPtrYamlConverter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(ObjectPtr);

        public object ReadYaml(IParser parser, Type type)
        {
            var objPtr = new ObjectPtr();

            if (parser.Current is Scalar scalar)
            {
                var value = scalar.Value ?? string.Empty;
                var normalized = value.Trim();

                if (string.IsNullOrEmpty(normalized) ||
                    normalized == "~" ||
                    normalized.Equals("null", StringComparison.OrdinalIgnoreCase) ||
                    normalized == FileId.NullFileId ||
                    normalized == InstanceId.NullInstanceId)
                {
                    parser.MoveNext();
                    return objPtr;
                }
            }

            parser.Consume<MappingStart>();

            while (parser.TryConsume<Scalar>(out var key))
            {
                switch (key.Value)
                {
                    case "fileId":
                        objPtr.FileId = new FileId { Value = parser.Consume<Scalar>().Value };
                        break;
                    case "instanceId":
                        objPtr.InstanceId = new InstanceId { Value = parser.Consume<Scalar>().Value };
                        break;
                    default:
                        throw new YamlException($"Unexpected key: {key.Value}");
                }
            }

            parser.Consume<MappingEnd>();
            return objPtr;
        }

        public void WriteYaml(IEmitter emitter, object value, Type type)
        {
            var objPtr = (ObjectPtr)value;

            emitter.Emit(new MappingStart(null, null, false, MappingStyle.Block));

            emitter.Emit(new Scalar(null, "fileId"));
            emitter.Emit(new Scalar(null, objPtr.FileId.Value.ToString()));

            emitter.Emit(new Scalar(null, "instanceId"));
            emitter.Emit(new Scalar(null, objPtr.InstanceId.Value.ToString()));

            emitter.Emit(new MappingEnd());
        }
    }
};
