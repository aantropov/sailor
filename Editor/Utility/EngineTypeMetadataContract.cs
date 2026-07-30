#nullable enable

using System.Collections;
using YamlDotNet.Core;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace SailorEngine;

public sealed class EngineTypeMetadataContract
{
    public uint? MetadataVersion { get; set; }
    public string? ModuleName { get; set; }
    public long? TimeStamp { get; set; }
    public List<EngineTypeMetadataType> EngineTypes { get; set; } = [];
    public List<EngineTypeMetadataDefaults> Cdos { get; set; } = [];
    public List<Dictionary<string, List<string>>> Enums { get; set; } = [];
    public List<EngineTypeMetadataAssetType> AssetTypes { get; set; } = [];
}

public sealed class EngineTypeMetadataType
{
    public string Typename { get; set; } = string.Empty;
    public string Base { get; set; } = string.Empty;
    public Dictionary<string, string> Properties { get; set; } = [];
    public Dictionary<string, EngineTypeMetadataPropertyRange> PropertyRanges { get; set; } = [];
    public List<string> ReadOnlyProperties { get; set; } = [];
}

public sealed class EngineTypeMetadataPropertyRange
{
    public double? Min { get; set; }
    public double? Max { get; set; }
}

public sealed record NumericPropertyRange(double Minimum, double Maximum)
{
    public double Clamp(double value) => Math.Clamp(value, Minimum, Maximum);

    public float Clamp(float value) => (float)Clamp((double)value);

    public int Clamp(int value) => (int)Clamp((double)value);

    public uint Clamp(uint value) => (uint)Clamp((double)value);

    public int SnapInt32(double value)
        => (int)Math.Round(Clamp(value), MidpointRounding.AwayFromZero);

    public uint SnapUInt32(double value)
        => (uint)Math.Round(Clamp(value), MidpointRounding.AwayFromZero);
}

public sealed class EngineTypeMetadataDefaults
{
    public string Typename { get; set; } = string.Empty;
    public Dictionary<string, object> DefaultValues { get; set; } = [];
}

public sealed class EngineTypeMetadataAssetType
{
    public string Typename { get; set; } = string.Empty;
    public List<string> Extensions { get; set; } = [];
    public object? Properties { get; set; }
}

public sealed class EngineTypeMetadataAssetProperty
{
    public string Name { get; set; } = string.Empty;
    public string Type { get; set; } = string.Empty;
}

public static class EditorAddComponentTypeContract
{
    const string ComponentRootTypeName = "Sailor::Component";

    static readonly HashSet<string> HiddenTypeRoots = new(StringComparer.Ordinal)
    {
        "Sailor::EditorComponent",
        "Sailor::TestComponent",
        "Sailor::TestCaseComponent",
        "Sailor::PerformanceTestSetupComponent"
    };

    public static bool IsAddable(
        string? typeName,
        Func<string, string?> resolveBaseType)
    {
        ArgumentNullException.ThrowIfNull(resolveBaseType);

        if (string.IsNullOrWhiteSpace(typeName) ||
            string.Equals(typeName, ComponentRootTypeName, StringComparison.Ordinal))
        {
            return false;
        }

        var currentTypeName = typeName;
        var visited = new HashSet<string>(StringComparer.Ordinal) { currentTypeName };
        while (true)
        {
            if (HiddenTypeRoots.Contains(currentTypeName))
                return false;

            var baseTypeName = resolveBaseType(currentTypeName);
            if (string.IsNullOrWhiteSpace(baseTypeName))
                return false;
            if (string.Equals(baseTypeName, ComponentRootTypeName, StringComparison.Ordinal))
                return true;
            if (!visited.Add(baseTypeName))
                return false;

            currentTypeName = baseTypeName;
        }
    }
}

public sealed class EditorTypeCatalogSnapshot
{
    const string ComponentRootTypeName = "Sailor::Component";

    readonly Dictionary<string, EngineTypeMetadataType> types;
    readonly Dictionary<string, EngineTypeMetadataDefaults> defaults;

    EditorTypeCatalogSnapshot(
        EngineTypeMetadataContract document,
        Dictionary<string, EngineTypeMetadataType> types,
        Dictionary<string, EngineTypeMetadataDefaults> defaults)
    {
        Document = document;
        this.types = types;
        this.defaults = defaults;
    }

    public EngineTypeMetadataContract Document { get; }

    public static EditorTypeCatalogSnapshot Parse(string yaml)
    {
        if (string.IsNullOrWhiteSpace(yaml))
            throw new InvalidDataException("The editor type catalog is empty.");

        EngineTypeMetadataContract document;
        try
        {
            document = new DeserializerBuilder()
                .WithNamingConvention(CamelCaseNamingConvention.Instance)
                .IgnoreUnmatchedProperties()
                .Build()
                .Deserialize<EngineTypeMetadataContract>(yaml)
                ?? throw new InvalidDataException("The editor type catalog document is empty.");
            ValidatePropertyRangeYamlShape(yaml);
        }
        catch (YamlException ex)
        {
            throw new InvalidDataException($"The editor type catalog is invalid YAML: {ex.Message}", ex);
        }

        document.EngineTypes ??= [];
        document.Cdos ??= [];
        document.Enums ??= [];
        document.AssetTypes ??= [];

        var types = BuildUniqueIndex(
            document.EngineTypes,
            type => type.Typename,
            "type");
        var defaults = BuildUniqueIndex(
            document.Cdos,
            cdo => cdo.Typename,
            "default object");
        var assetTypes = BuildUniqueIndex(
            document.AssetTypes,
            assetType => assetType.Typename,
            "asset type");

        foreach (var cdoName in defaults.Keys)
        {
            if (!types.ContainsKey(cdoName))
                throw new InvalidDataException($"Default object '{cdoName}' has no matching reflected type.");
        }

        foreach (var defaultObject in defaults.Values)
            defaultObject.DefaultValues ??= [];

        foreach (var type in types.Values)
        {
            type.Properties ??= [];
            if (type.PropertyRanges is null)
            {
                throw new InvalidDataException(
                    $"Reflected type '{type.Typename}' contains a null propertyRanges map.");
            }
            type.ReadOnlyProperties ??= [];
            foreach (var property in type.Properties)
            {
                if (string.IsNullOrWhiteSpace(property.Key) || string.IsNullOrWhiteSpace(property.Value))
                {
                    throw new InvalidDataException(
                        $"Reflected type '{type.Typename}' contains a property with an empty name or type.");
                }
            }

            foreach (var propertyRange in type.PropertyRanges)
            {
                ValidatePropertyRange(type, propertyRange.Key, propertyRange.Value);
            }

            var readOnlyProperties = new HashSet<string>(StringComparer.Ordinal);
            foreach (var propertyName in type.ReadOnlyProperties)
            {
                if (string.IsNullOrWhiteSpace(propertyName) ||
                    !readOnlyProperties.Add(propertyName) ||
                    type.Properties.ContainsKey(propertyName))
                {
                    throw new InvalidDataException(
                        $"Reflected type '{type.Typename}' contains an invalid read-only property '{propertyName}'.");
                }
            }
        }

        var enumValuesByName = new Dictionary<string, IReadOnlyCollection<string>>(StringComparer.Ordinal);
        foreach (var enumGroup in document.Enums)
        {
            if (enumGroup is null || enumGroup.Count == 0)
                throw new InvalidDataException("The editor type catalog contains an empty enum group.");

            foreach (var enumEntry in enumGroup)
            {
                if (string.IsNullOrWhiteSpace(enumEntry.Key))
                    throw new InvalidDataException("The editor type catalog contains an enum with an empty name.");
                if (enumEntry.Value is null || enumEntry.Value.Count == 0)
                    throw new InvalidDataException($"Enum '{enumEntry.Key}' has no values in the editor type catalog.");

                var uniqueValues = new HashSet<string>(StringComparer.Ordinal);
                foreach (var enumValue in enumEntry.Value)
                {
                    if (string.IsNullOrWhiteSpace(enumValue) || !uniqueValues.Add(enumValue))
                    {
                        throw new InvalidDataException(
                            $"Enum '{enumEntry.Key}' contains an empty or duplicate value '{enumValue}'.");
                    }
                }

                if (!enumValuesByName.TryAdd(enumEntry.Key, uniqueValues))
                    throw new InvalidDataException($"Duplicate enum '{enumEntry.Key}' in the editor type catalog.");
            }
        }

        foreach (var type in types.Values)
        {
            foreach (var property in type.Properties)
            {
                if (property.Value.StartsWith("enum ", StringComparison.Ordinal) &&
                    !enumValuesByName.ContainsKey(property.Value))
                {
                    throw new InvalidDataException(
                        $"Reflected property '{type.Typename}.{property.Key}' references missing enum metadata '{property.Value}'.");
                }

                if (enumValuesByName.TryGetValue(property.Value, out var allowedValues) &&
                    defaults.TryGetValue(type.Typename, out var defaultObject) &&
                    defaultObject.DefaultValues.TryGetValue(property.Key, out var rawDefault))
                {
                    var value = Convert.ToString(rawDefault, System.Globalization.CultureInfo.InvariantCulture) ?? string.Empty;
                    EditorComponentPropertyContract.ValidateEnumValue(
                        type.Typename,
                        property.Key,
                        property.Value,
                        value,
                        allowedValues);
                }
            }
        }

        var extensions = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var assetType in assetTypes.Values)
        {
            assetType.Extensions ??= [];
            foreach (var extension in assetType.Extensions)
            {
                var normalized = extension?.Trim().TrimStart('.');
                if (string.IsNullOrWhiteSpace(normalized))
                    throw new InvalidDataException($"Asset type '{assetType.Typename}' contains an empty extension.");
                if (!extensions.Add(normalized))
                    throw new InvalidDataException($"Duplicate asset extension '{normalized}' in the editor type catalog.");
            }
        }

        return new EditorTypeCatalogSnapshot(document, types, defaults);
    }

    static void ValidatePropertyRangeYamlShape(string yaml)
    {
        var yamlStream = new YamlStream();
        using var reader = new StringReader(yaml);
        yamlStream.Load(reader);
        if (yamlStream.Documents.Count == 0 ||
            yamlStream.Documents[0].RootNode is not YamlMappingNode root ||
            !TryGetMappingValue(root, "engineTypes", out var engineTypesNode) ||
            engineTypesNode is not YamlSequenceNode engineTypes)
        {
            return;
        }

        foreach (var engineTypeNode in engineTypes.Children)
        {
            if (engineTypeNode is not YamlMappingNode engineType ||
                !TryGetMappingValue(engineType, "propertyRanges", out var propertyRangesNode))
            {
                continue;
            }

            var typeName = TryGetMappingValue(engineType, "typename", out var typeNameNode) &&
                typeNameNode is YamlScalarNode typeNameScalar
                ? typeNameScalar.Value ?? string.Empty
                : string.Empty;
            if (propertyRangesNode is not YamlMappingNode propertyRanges)
            {
                throw new InvalidDataException(
                    $"Reflected type '{typeName}' must declare propertyRanges as a map.");
            }

            foreach (var propertyRange in propertyRanges.Children)
            {
                var propertyName = propertyRange.Key is YamlScalarNode propertyNameScalar
                    ? propertyNameScalar.Value ?? string.Empty
                    : string.Empty;
                var qualifiedPropertyName = $"{typeName}.{propertyName}";
                if (string.IsNullOrWhiteSpace(propertyName) ||
                    propertyRange.Value is not YamlMappingNode range ||
                    range.Children.Count != 2 ||
                    !HasScalarMappingValue(range, "min") ||
                    !HasScalarMappingValue(range, "max"))
                {
                    throw new InvalidDataException(
                        $"Property range '{qualifiedPropertyName}' must contain exactly scalar min and max fields.");
                }
            }
        }
    }

    static bool HasScalarMappingValue(YamlMappingNode mapping, string key)
        => TryGetMappingValue(mapping, key, out var value) &&
            value is YamlScalarNode;

    static bool TryGetMappingValue(
        YamlMappingNode mapping,
        string key,
        out YamlNode value)
    {
        foreach (var child in mapping.Children)
        {
            if (child.Key is YamlScalarNode scalar &&
                string.Equals(scalar.Value, key, StringComparison.Ordinal))
            {
                value = child.Value;
                return true;
            }
        }

        value = null!;
        return false;
    }

    static void ValidatePropertyRange(
        EngineTypeMetadataType type,
        string propertyName,
        EngineTypeMetadataPropertyRange? propertyRange)
    {
        var qualifiedPropertyName = $"{type.Typename}.{propertyName}";
        if (string.IsNullOrWhiteSpace(propertyName) ||
            !type.Properties.TryGetValue(propertyName, out var propertyType))
        {
            throw new InvalidDataException(
                $"Property range '{qualifiedPropertyName}' does not reference a reflected writable property.");
        }
        if (propertyRange?.Min is not double minimum ||
            propertyRange.Max is not double maximum)
        {
            throw new InvalidDataException(
                $"Property range '{qualifiedPropertyName}' must declare both min and max.");
        }
        if (!double.IsFinite(minimum) ||
            !double.IsFinite(maximum) ||
            minimum >= maximum)
        {
            throw new InvalidDataException(
                $"Property range '{qualifiedPropertyName}' must contain finite bounds with min less than max.");
        }

        switch (propertyType)
        {
            case "float":
                if (minimum < -float.MaxValue || maximum > float.MaxValue)
                {
                    throw new InvalidDataException(
                        $"Property range '{qualifiedPropertyName}' exceeds the supported float bounds.");
                }
                break;
            case "int32":
                ValidateIntegralRange(
                    qualifiedPropertyName,
                    minimum,
                    maximum,
                    int.MinValue,
                    int.MaxValue,
                    "int32");
                break;
            case "uint32":
                ValidateIntegralRange(
                    qualifiedPropertyName,
                    minimum,
                    maximum,
                    uint.MinValue,
                    uint.MaxValue,
                    "uint32");
                break;
            default:
                throw new InvalidDataException(
                    $"Property range '{qualifiedPropertyName}' cannot be applied to '{propertyType}'.");
        }
    }

    static void ValidateIntegralRange(
        string qualifiedPropertyName,
        double minimum,
        double maximum,
        double supportedMinimum,
        double supportedMaximum,
        string propertyType)
    {
        if (minimum != Math.Truncate(minimum) ||
            maximum != Math.Truncate(maximum) ||
            minimum < supportedMinimum ||
            maximum > supportedMaximum)
        {
            throw new InvalidDataException(
                $"Property range '{qualifiedPropertyName}' must use representable integral {propertyType} bounds.");
        }
    }

    public bool TryGetType(string typeName, out EngineTypeMetadataType type)
        => types.TryGetValue(typeName, out type!);

    public bool IsKnownReadOnlyProperty(string typeName, string propertyName)
    {
        return types.TryGetValue(typeName, out var type) &&
            !type.Properties.ContainsKey(propertyName) &&
            (type.ReadOnlyProperties.Contains(propertyName, StringComparer.Ordinal) ||
                (defaults.TryGetValue(typeName, out var defaultObject) &&
                    defaultObject.DefaultValues.ContainsKey(propertyName)));
    }

    public bool IsComponentType(string typeName)
    {
        if (string.IsNullOrWhiteSpace(typeName) ||
            string.Equals(typeName, ComponentRootTypeName, StringComparison.Ordinal) ||
            !types.TryGetValue(typeName, out var current))
        {
            return false;
        }

        var visited = new HashSet<string>(StringComparer.Ordinal) { typeName };
        while (!string.IsNullOrWhiteSpace(current.Base))
        {
            if (string.Equals(current.Base, ComponentRootTypeName, StringComparison.Ordinal))
                return true;
            if (!visited.Add(current.Base) || !types.TryGetValue(current.Base, out current))
                return false;
        }

        return false;
    }

    public IReadOnlyList<string> GetComponentTypeNames()
        => types.Keys
            .Where(IsComponentType)
            .OrderBy(typeName => typeName, StringComparer.Ordinal)
            .ToArray();

    public bool IsAddableComponentType(string typeName)
        => EditorAddComponentTypeContract.IsAddable(
            typeName,
            currentTypeName => types.TryGetValue(currentTypeName, out var current)
                ? current.Base
                : null);

    public IReadOnlyList<string> GetAddableComponentTypeNames()
        => types.Keys
            .Where(IsAddableComponentType)
            .OrderBy(typeName => typeName, StringComparer.Ordinal)
            .ToArray();

    static Dictionary<string, T> BuildUniqueIndex<T>(
        IEnumerable<T> values,
        Func<T, string> getName,
        string identityKind)
    {
        var result = new Dictionary<string, T>(StringComparer.Ordinal);
        foreach (var value in values)
        {
            if (value is null)
                throw new InvalidDataException($"The editor type catalog contains an empty {identityKind} entry.");

            var name = getName(value);
            if (string.IsNullOrWhiteSpace(name))
                throw new InvalidDataException($"The editor type catalog contains a {identityKind} with an empty name.");
            if (!result.TryAdd(name, value))
                throw new InvalidDataException($"Duplicate {identityKind} '{name}' in the editor type catalog.");
        }

        return result;
    }
}

public static class EditorTypeMetadataValueCodec
{
    public static IReadOnlyList<float> ParseFloatSequence(
        object? value,
        int expectedCount,
        string valueName)
    {
        if (expectedCount <= 0)
            throw new ArgumentOutOfRangeException(nameof(expectedCount));
        if (value is string || value is not IEnumerable sequence)
            throw new InvalidDataException($"Metadata value '{valueName}' must be a numeric sequence.");

        var result = new List<float>(expectedCount);
        try
        {
            foreach (var item in sequence)
                result.Add(Convert.ToSingle(item, System.Globalization.CultureInfo.InvariantCulture));
        }
        catch (Exception ex) when (ex is FormatException or InvalidCastException or OverflowException)
        {
            throw new InvalidDataException($"Metadata value '{valueName}' contains a non-numeric element.", ex);
        }

        if (result.Count != expectedCount)
        {
            throw new InvalidDataException(
                $"Metadata value '{valueName}' must contain exactly {expectedCount} elements.");
        }

        return result;
    }
}
