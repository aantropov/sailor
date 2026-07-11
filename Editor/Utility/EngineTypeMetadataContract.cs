#nullable enable

using System.Collections;
using YamlDotNet.Core;
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

public sealed class EditorTypeCatalogSnapshot
{
    const string ComponentRootTypeName = "Sailor::Component";

    readonly Dictionary<string, EngineTypeMetadataType> types;

    EditorTypeCatalogSnapshot(
        EngineTypeMetadataContract document,
        Dictionary<string, EngineTypeMetadataType> types)
    {
        Document = document;
        this.types = types;
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

        foreach (var type in types.Values)
        {
            type.Properties ??= [];
            foreach (var property in type.Properties)
            {
                if (string.IsNullOrWhiteSpace(property.Key) || string.IsNullOrWhiteSpace(property.Value))
                {
                    throw new InvalidDataException(
                        $"Reflected type '{type.Typename}' contains a property with an empty name or type.");
                }
            }
        }

        var enumNames = new HashSet<string>(StringComparer.Ordinal);
        foreach (var enumGroup in document.Enums)
        {
            if (enumGroup is null)
                throw new InvalidDataException("The editor type catalog contains an empty enum group.");

            foreach (var enumEntry in enumGroup)
            {
                if (string.IsNullOrWhiteSpace(enumEntry.Key))
                    throw new InvalidDataException("The editor type catalog contains an enum with an empty name.");
                if (!enumNames.Add(enumEntry.Key))
                    throw new InvalidDataException($"Duplicate enum '{enumEntry.Key}' in the editor type catalog.");
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

        return new EditorTypeCatalogSnapshot(document, types);
    }

    public bool TryGetType(string typeName, out EngineTypeMetadataType type)
        => types.TryGetValue(typeName, out type!);

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
