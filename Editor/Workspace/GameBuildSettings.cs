#nullable enable

using System.Text.Json;
using System.Text.Json.Serialization;

namespace SailorEditor.Workspace;

public enum GameBuildPlatform { Windows, Linux, MacOS }
public enum GameAssetPackaging { Folders, Zip }
public enum GameArchiveCompression { Stored, Fastest, Balanced, Smallest }

public sealed record GamePlatformBuildSettings
{
    public GameAssetPackaging Assets { get; init; } = GameAssetPackaging.Folders;
    public GameArchiveCompression Compression { get; init; } = GameArchiveCompression.Balanced;
    public bool IncludeInitialCache { get; init; }
    public string OutputDirectory { get; init; } = string.Empty;
    public string ToolchainFile { get; init; } = string.Empty;
    public string EngineBuildDirectory { get; init; } = string.Empty;
}

public sealed record GameBuildSettings
{
    [JsonRequired] public int SchemaVersion { get; init; } = 1;
    public string Configuration { get; init; } = "Release";
    public string StartupScene { get; init; } = string.Empty;
    public Dictionary<GameBuildPlatform, GamePlatformBuildSettings> Platforms { get; init; } =
        Enum.GetValues<GameBuildPlatform>().ToDictionary(platform => platform, _ => new GamePlatformBuildSettings());

    public static GameBuildPlatform HostPlatform => OperatingSystem.IsWindows() ? GameBuildPlatform.Windows :
        OperatingSystem.IsMacOS() || OperatingSystem.IsMacCatalyst() ? GameBuildPlatform.MacOS :
        OperatingSystem.IsLinux() ? GameBuildPlatform.Linux : throw new PlatformNotSupportedException();

    public void Validate()
    {
        if (SchemaVersion != 1 || Configuration is not ("Debug" or "Release") || StartupScene is null || Platforms is null ||
            Platforms.Count != 3 || Enum.GetValues<GameBuildPlatform>().Any(platform => !Platforms.ContainsKey(platform)))
        {
            throw new InvalidDataException("Build settings require schemaVersion 1, Debug/Release and all three desktop platforms.");
        }
        foreach (var settings in Platforms.Values)
        {
            if (settings is null || !Enum.IsDefined(settings.Assets) || !Enum.IsDefined(settings.Compression) ||
                settings.OutputDirectory is null || settings.ToolchainFile is null || settings.EngineBuildDirectory is null)
            {
                throw new InvalidDataException("Invalid platform build options.");
            }
        }
    }
}

public static class GameBuildSettingsStore
{
    public const string FileName = "BuildSettings.json";
    static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        Converters = { new JsonStringEnumConverter(allowIntegerValues: false) }
    };

    public static GameBuildSettings Load(string workspace)
    {
        var path = Path.Combine(workspace, FileName);
        if (!File.Exists(path))
        {
            return new GameBuildSettings();
        }
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        ValidateUniqueKeys(document.RootElement);
        var settings = document.RootElement.Deserialize<GameBuildSettings>(Options)
            ?? throw new InvalidDataException("Build settings are empty.");
        settings.Validate();
        return settings;
    }

    public static void Save(string workspace, GameBuildSettings settings)
    {
        settings.Validate();
        var path = Path.Combine(workspace, FileName);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, JsonSerializer.Serialize(settings, Options) + "\n");
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            File.Delete(temporary);
        }
    }

    static void ValidateUniqueKeys(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var keys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var property in element.EnumerateObject())
            {
                if (!keys.Add(property.Name))
                {
                    throw new InvalidDataException($"Duplicate build settings field: {property.Name}");
                }
                ValidateUniqueKeys(property.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (var value in element.EnumerateArray())
            {
                ValidateUniqueKeys(value);
            }
        }
    }
}
