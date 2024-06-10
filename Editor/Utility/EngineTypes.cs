
using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.ViewModels;
using System.Numerics;
using YamlDotNet.Core.Events;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization;

namespace SailorEngine
{
    public class PropertyBase
    {
        public string Typename { get; set; }
    }

    public class Property<T> : PropertyBase
    {
        public Property() { Typename = typeof(T).Name; }
        public T DefaultValue { get; set; } = default;
    };

    public class FloatProperty : Property<float> { }
    public class Vec4Property : Property<SailorEditor.Vec4> { }
    public class Vec3Property : Property<SailorEditor.Vec3> { }
    public class Vec2Property : Property<SailorEditor.Vec2> { }
    public class FileIdProperty : Property<AssetUID> { }
    public class InstanceIdProperty : Property<string> { }

    public class ComponentType
    {
        public string Name { get; set; }
        public Dictionary<string, PropertyBase> Properties { get; set; } = new();
    };

    class Helper
    {
        public static Dictionary<string, ComponentType> ReadEngineTypes(string filePath)
        {
            var parsed = new Dictionary<string, Dictionary<string, string>>();
            var componentTypes = new Dictionary<string, ComponentType>();

            using (var yamlAssetInfo = new FileStream(filePath, FileMode.Open))
            using (var reader = new StreamReader(yamlAssetInfo))
            {
                var yaml = new YamlStream();
                yaml.Load(reader);

                var root = (YamlMappingNode)yaml.Documents[0].RootNode;
                var engineTypesNode = (YamlSequenceNode)root.Children[new YamlScalarNode("engineTypes")];

                foreach (YamlMappingNode typeNode in engineTypesNode.Children)
                {
                    var typeName = typeNode.Children[new YamlScalarNode("typename")].ToString();
                    var propertiesNode = (YamlMappingNode)typeNode.Children[new YamlScalarNode("properties")];

                    var properties = new Dictionary<string, string>();
                    foreach (var property in propertiesNode.Children)
                    {
                        properties[property.Key.ToString()] = property.Value.ToString();
                    }

                    parsed[typeName] = properties;
                }
            }

            foreach (var component in parsed)
            {
                var newComponent = new ComponentType();
                newComponent.Name = component.Key;

                foreach (var property in component.Value)
                {
                    PropertyBase newProperty = null;

                    if (property.Value == "struct glm::vec<2,float,0>")
                    {
                        newProperty = new Vec2Property();
                    }
                    else if (property.Value == "struct glm::vec<3,float,0>")
                    {
                        newProperty = new Vec3Property();
                    }
                    else if (property.Value == "struct glm::vec<4,float,0>")
                    {
                        newProperty = new Vec4Property();
                    }
                    else if (property.Value == "float")
                    {
                        newProperty = new FloatProperty();
                    }
                    else if (property.Value == "class Sailor::TObjectPtr<class Sailor::Model>")
                    {
                        newProperty = new FileIdProperty();
                    }
                    else if (property.Value.Contains("TObjectPtr"))
                    {
                        newProperty = new InstanceIdProperty();
                    }

                    newComponent.Properties[property.Key] = newProperty;
                }

                componentTypes[component.Key] = newComponent;
            }

            return componentTypes;
        }
    }

    public enum FillMode
    {
        Fill = 0,
        Line = 1,
        Point = 2
    };

    public enum CullMode
    {
        None = 0,
        Front = 0x00000001,
        Back = 0x00000002,
        FrontAndBack = 0x00000003
    };

    public enum BlendMode
    {
        None = 0,
        Additive = 0x00000001,
        AlphaBlending = 0x00000002,
        Multiply = 0x00000003
    };

    public enum TextureFiltration
    {
        Nearest = 0,
        Linear,
        Bicubic
    };

    public enum TextureClamping
    {
        Clamp = 0,
        Repeat
    };

    public enum TextureFormat
    {
        UNDEFINED = 0,
        R4G4_UNORM_PACK8 = 1,
        R4G4B4A4_UNORM_PACK16 = 2,
        B4G4R4A4_UNORM_PACK16 = 3,
        R5G6B5_UNORM_PACK16 = 4,
        B5G6R5_UNORM_PACK16 = 5,
        R5G5B5A1_UNORM_PACK16 = 6,
        B5G5R5A1_UNORM_PACK16 = 7,
        A1R5G5B5_UNORM_PACK16 = 8,
        R8_UNORM = 9,
        R8_SNORM = 10,
        R8_USCALED = 11,
        R8_SSCALED = 12,
        R8_UINT = 13,
        R8_SINT = 14,
        R8_SRGB = 15,
        R8G8_UNORM = 16,
        R8G8_SNORM = 17,
        R8G8_USCALED = 18,
        R8G8_SSCALED = 19,
        R8G8_UINT = 20,
        R8G8_SINT = 21,
        R8G8_SRGB = 22,
        R8G8B8_UNORM = 23,
        R8G8B8_SNORM = 24,
        R8G8B8_USCALED = 25,
        R8G8B8_SSCALED = 26,
        R8G8B8_UINT = 27,
        R8G8B8_SINT = 28,
        R8G8B8_SRGB = 29,
        B8G8R8_UNORM = 30,
        B8G8R8_SNORM = 31,
        B8G8R8_USCALED = 32,
        B8G8R8_SSCALED = 33,
        B8G8R8_UINT = 34,
        B8G8R8_SINT = 35,
        B8G8R8_SRGB = 36,
        R8G8B8A8_UNORM = 37,
        R8G8B8A8_SNORM = 38,
        R8G8B8A8_USCALED = 39,
        R8G8B8A8_SSCALED = 40,
        R8G8B8A8_UINT = 41,
        R8G8B8A8_SINT = 42,
        R8G8B8A8_SRGB = 43,
        B8G8R8A8_UNORM = 44,
        B8G8R8A8_SNORM = 45,
        B8G8R8A8_USCALED = 46,
        B8G8R8A8_SSCALED = 47,
        B8G8R8A8_UINT = 48,
        B8G8R8A8_SINT = 49,
        B8G8R8A8_SRGB = 50,
        A8B8G8R8_UNORM_PACK32 = 51,
        A8B8G8R8_SNORM_PACK32 = 52,
        A8B8G8R8_USCALED_PACK32 = 53,
        A8B8G8R8_SSCALED_PACK32 = 54,
        A8B8G8R8_UINT_PACK32 = 55,
        A8B8G8R8_SINT_PACK32 = 56,
        A8B8G8R8_SRGB_PACK32 = 57,
        A2R10G10B10_UNORM_PACK32 = 58,
        A2R10G10B10_SNORM_PACK32 = 59,
        A2R10G10B10_USCALED_PACK32 = 60,
        A2R10G10B10_SSCALED_PACK32 = 61,
        A2R10G10B10_UINT_PACK32 = 62,
        A2R10G10B10_SINT_PACK32 = 63,
        A2B10G10R10_UNORM_PACK32 = 64,
        A2B10G10R10_SNORM_PACK32 = 65,
        A2B10G10R10_USCALED_PACK32 = 66,
        A2B10G10R10_SSCALED_PACK32 = 67,
        A2B10G10R10_UINT_PACK32 = 68,
        A2B10G10R10_SINT_PACK32 = 69,
        R16_UNORM = 70,
        R16_SNORM = 71,
        R16_USCALED = 72,
        R16_SSCALED = 73,
        R16_UINT = 74,
        R16_SINT = 75,
        R16_SFLOAT = 76,
        R16G16_UNORM = 77,
        R16G16_SNORM = 78,
        R16G16_USCALED = 79,
        R16G16_SSCALED = 80,
        R16G16_UINT = 81,
        R16G16_SINT = 82,
        R16G16_SFLOAT = 83,
        R16G16B16_UNORM = 84,
        R16G16B16_SNORM = 85,
        R16G16B16_USCALED = 86,
        R16G16B16_SSCALED = 87,
        R16G16B16_UINT = 88,
        R16G16B16_SINT = 89,
        R16G16B16_SFLOAT = 90,
        R16G16B16A16_UNORM = 91,
        R16G16B16A16_SNORM = 92,
        R16G16B16A16_USCALED = 93,
        R16G16B16A16_SSCALED = 94,
        R16G16B16A16_UINT = 95,
        R16G16B16A16_SINT = 96,
        R16G16B16A16_SFLOAT = 97,
        R32_UINT = 98,
        R32_SINT = 99,
        R32_SFLOAT = 100,
        R32G32_UINT = 101,
        R32G32_SINT = 102,
        R32G32_SFLOAT = 103,
        R32G32B32_UINT = 104,
        R32G32B32_SINT = 105,
        R32G32B32_SFLOAT = 106,
        R32G32B32A32_UINT = 107,
        R32G32B32A32_SINT = 108,
        R32G32B32A32_SFLOAT = 109,
        R64_UINT = 110,
        R64_SINT = 111,
        R64_SFLOAT = 112,
        R64G64_UINT = 113,
        R64G64_SINT = 114,
        R64G64_SFLOAT = 115,
        R64G64B64_UINT = 116,
        R64G64B64_SINT = 117,
        R64G64B64_SFLOAT = 118,
        R64G64B64A64_UINT = 119,
        R64G64B64A64_SINT = 120,
        R64G64B64A64_SFLOAT = 121,
        B10G11R11_UFLOAT_PACK32 = 122,
        E5B9G9R9_UFLOAT_PACK32 = 123,
        D16_UNORM = 124,
        X8_D24_UNORM_PACK32 = 125,
        D32_SFLOAT = 126,
        S8_UINT = 127,
        D16_UNORM_S8_UINT = 128,
        D24_UNORM_S8_UINT = 129,
        D32_SFLOAT_S8_UINT = 130,
        BC1_RGB_UNORM_BLOCK = 131,
        BC1_RGB_SRGB_BLOCK = 132,
        BC1_RGBA_UNORM_BLOCK = 133,
        BC1_RGBA_SRGB_BLOCK = 134,
        BC2_UNORM_BLOCK = 135,
        BC2_SRGB_BLOCK = 136,
        BC3_UNORM_BLOCK = 137,
        BC3_SRGB_BLOCK = 138,
        BC4_UNORM_BLOCK = 139,
        BC4_SNORM_BLOCK = 140,
        BC5_UNORM_BLOCK = 141,
        BC5_SNORM_BLOCK = 142,
        BC6H_UFLOAT_BLOCK = 143,
        BC6H_SFLOAT_BLOCK = 144,
        BC7_UNORM_BLOCK = 145,
        BC7_SRGB_BLOCK = 146,
        ETC2_R8G8B8_UNORM_BLOCK = 147,
        ETC2_R8G8B8_SRGB_BLOCK = 148,
        ETC2_R8G8B8A1_UNORM_BLOCK = 149,
        ETC2_R8G8B8A1_SRGB_BLOCK = 150,
        ETC2_R8G8B8A8_UNORM_BLOCK = 151,
        ETC2_R8G8B8A8_SRGB_BLOCK = 152,
        EAC_R11_UNORM_BLOCK = 153,
        EAC_R11_SNORM_BLOCK = 154,
        EAC_R11G11_UNORM_BLOCK = 155,
        EAC_R11G11_SNORM_BLOCK = 156,
        ASTC_4x4_UNORM_BLOCK = 157,
        ASTC_4x4_SRGB_BLOCK = 158,
        ASTC_5x4_UNORM_BLOCK = 159,
        ASTC_5x4_SRGB_BLOCK = 160,
        ASTC_5x5_UNORM_BLOCK = 161,
        ASTC_5x5_SRGB_BLOCK = 162,
        ASTC_6x5_UNORM_BLOCK = 163,
        ASTC_6x5_SRGB_BLOCK = 164,
        ASTC_6x6_UNORM_BLOCK = 165,
        ASTC_6x6_SRGB_BLOCK = 166,
        ASTC_8x5_UNORM_BLOCK = 167,
        ASTC_8x5_SRGB_BLOCK = 168,
        ASTC_8x6_UNORM_BLOCK = 169,
        ASTC_8x6_SRGB_BLOCK = 170,
        ASTC_8x8_UNORM_BLOCK = 171,
        ASTC_8x8_SRGB_BLOCK = 172,
        ASTC_10x5_UNORM_BLOCK = 173,
        ASTC_10x5_SRGB_BLOCK = 174,
        ASTC_10x6_UNORM_BLOCK = 175,
        ASTC_10x6_SRGB_BLOCK = 176,
        ASTC_10x8_UNORM_BLOCK = 177,
        ASTC_10x8_SRGB_BLOCK = 178,
        ASTC_10x10_UNORM_BLOCK = 179,
        ASTC_10x10_SRGB_BLOCK = 180,
        ASTC_12x10_UNORM_BLOCK = 181,
        ASTC_12x10_SRGB_BLOCK = 182,
        ASTC_12x12_UNORM_BLOCK = 183,
        ASTC_12x12_SRGB_BLOCK = 184
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

        public static implicit operator Vec4(Vector4 value) => new Vec4 { X = value.X, Y = value.Y, Z = value.Z, W = value.W };
        public static implicit operator Vector4(Vec4 uniform) => new Vector4(uniform.X, uniform.Y, uniform.Z, uniform.W);
        public object Clone() => new Vec4 { X = X, Y = Y, Z = Z, W = W };
        public override string ToString() => $"<{X} {Y} {Z} {W}>";
        public int CompareTo(Vec4 other) => X.CompareTo(other.X) + Y.CompareTo(other.Y) + Z.CompareTo(other.Z) + W.CompareTo(other.W);

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

        public static implicit operator Vec3(Vector3 value) => new Vec3 { X = value.X, Y = value.Y, Z = value.Z };
        public static implicit operator Vector3(Vec3 uniform) => new Vector3(uniform.X, uniform.Y, uniform.Z);
        public object Clone() => new Vec3 { X = X, Y = Y, Z = Z };
        public override string ToString() => $"<{X} {Y} {Z}>";
        public int CompareTo(Vec3 other) => X.CompareTo(other.X) + Y.CompareTo(other.Y) + Z.CompareTo(other.Z);

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

        public static implicit operator Vec2(Vector2 value) => new Vec2 { X = value.X, Y = value.Y };
        public static implicit operator Vector2(Vec2 uniform) => new Vector2(uniform.X, uniform.Y);
        public object Clone() => new Vec2 { X = X, Y = Y };
        public override string ToString() => $"<{X} {Y}>";
        public int CompareTo(Vec2 other) => X.CompareTo(other.X) + Y.CompareTo(other.Y);

        [ObservableProperty]
        float x = 0.0f;

        [ObservableProperty]
        float y = 0.0f;
    }

    public class Vec2Converter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec2);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.MoveNext();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value));
                }
                parser.MoveNext();
            }
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

    public class Vec3Converter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec3);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.MoveNext();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value));
                }
                parser.MoveNext();
            }
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

    public class Vec4Converter : IYamlTypeConverter
    {
        public bool Accepts(Type type) => type == typeof(Vec4);

        public object ReadYaml(IParser parser, Type type)
        {
            var list = new List<float>();
            parser.MoveNext();
            while (parser.Current is SequenceEnd == false)
            {
                if (parser.Current is Scalar scalar)
                {
                    list.Add(float.Parse(scalar.Value));
                }
                parser.MoveNext();
            }
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
};