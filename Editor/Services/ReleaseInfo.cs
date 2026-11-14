using System.Reflection;

namespace SailorEditor.Services;

public static class ReleaseInfo
{
    public const string ReleasesUrl = "https://github.com/aantropov/sailor/releases";
    public static string EditorVersion { get; } = typeof(ReleaseInfo).Assembly
        .GetCustomAttribute<AssemblyInformationalVersionAttribute>()!.InformationalVersion.Split('+')[0];
    public static string EngineVersion { get; } = typeof(ReleaseInfo).Assembly
        .GetCustomAttributes<AssemblyMetadataAttribute>()
        .Single(attribute => attribute.Key == "SailorEngineVersion").Value!;
    public static string DisplayText => $"Editor {EditorVersion} · Engine {EngineVersion}";
}
