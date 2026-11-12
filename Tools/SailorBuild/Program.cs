using SailorEditor.Workspace;

if (args.Length == 0 || args.Contains("--help"))
{
    Console.WriteLine("SailorBuild --engine SOURCE_OR_SDK --workspace PROJECT --output NEW_DIRECTORY\n" +
        "  [--configuration Debug|Release] [--platform Windows|Linux|MacOS]\n" +
        "  [--assets Folders|Zip] [--compression Stored|Fastest|Balanced|Smallest]\n" +
        "  [--initial-cache|--no-initial-cache] [--engine-build-dir DIRECTORY]\n" +
        "  [--no-build-runtime] [--save-settings] [--cmake EXECUTABLE]\n" +
        "Omitted packaging options come from the project's version-1 BuildSettings.json.");
    return args.Length == 0 ? 1 : 0;
}
using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, eventArgs) =>
{
    eventArgs.Cancel = true;
    cancellation.Cancel();
};
try
{
    var values = new Dictionary<string, string>(StringComparer.Ordinal);
    var switches = new HashSet<string>(StringComparer.Ordinal);
    var flags = new[] { "--initial-cache", "--no-initial-cache", "--no-build-runtime", "--save-settings" };
    var options = new[] { "--engine", "--workspace", "--output", "--configuration", "--platform", "--assets", "--compression", "--engine-build-dir", "--cmake" };
    for (var index = 0; index < args.Length; ++index)
    {
        var name = args[index];
        if (flags.Contains(name) && switches.Add(name))
        {
            continue;
        }
        if (!options.Contains(name) || index + 1 >= args.Length || !values.TryAdd(name, args[++index]))
        {
            throw new ArgumentException("Unknown, repeated or incomplete option: " + name);
        }
    }
    if (switches.Contains("--initial-cache") && switches.Contains("--no-initial-cache"))
    {
        throw new ArgumentException("Choose one initial-cache option.");
    }
    var workspace = Path.GetFullPath(values.GetValueOrDefault("--workspace", "."));
    var engine = Path.GetFullPath(values.GetValueOrDefault("--engine") ?? throw new ArgumentException("--engine is required."));
    var settings = GameBuildSettingsStore.Load(workspace);
    var platform = Parse<GameBuildPlatform>("--platform", GameBuildSettings.HostPlatform);
    var packaging = settings.Platforms[platform];
    var output = values.GetValueOrDefault("--output", packaging.OutputDirectory);
    if (string.IsNullOrWhiteSpace(output))
    {
        throw new ArgumentException("--output is required unless saved in BuildSettings.json.");
    }
    output = Path.GetFullPath(output, workspace);
    packaging = packaging with
    {
        Assets = Parse("--assets", packaging.Assets),
        Compression = Parse("--compression", packaging.Compression),
        IncludeInitialCache = switches.Contains("--initial-cache") || (!switches.Contains("--no-initial-cache") && packaging.IncludeInitialCache),
        OutputDirectory = output
    };
    settings = settings with { Configuration = values.GetValueOrDefault("--configuration", settings.Configuration) };
    settings.Platforms[platform] = packaging;
    settings.Validate();
    if (switches.Contains("--save-settings"))
    {
        GameBuildSettingsStore.Save(workspace, settings);
    }
    var request = new GameBuildRequest(workspace, engine, output, settings.Configuration, platform, packaging)
    {
        EngineBuildDirectory = values.GetValueOrDefault("--engine-build-dir"),
        CMakeExecutable = values.GetValueOrDefault("--cmake", "cmake"),
        BuildRuntime = !switches.Contains("--no-build-runtime")
    };
    var result = await new GameBuildService(new WorkspaceProcessRunner()).BuildAsync(request,
        new ConsoleProgress(), cancellation.Token);
    if (!result.Succeeded)
    {
        Console.Error.WriteLine(result.Error);
        return 1;
    }
    return 0;

    T Parse<T>(string option, T fallback) where T : struct, Enum
    {
        if (!values.TryGetValue(option, out var text))
        {
            return fallback;
        }
        if (!Enum.GetNames<T>().Contains(text, StringComparer.OrdinalIgnoreCase) ||
            !Enum.TryParse<T>(text, ignoreCase: true, out var parsed))
        {
            throw new ArgumentException("Invalid value for " + option + ": " + text);
        }
        return parsed;
    }
}
catch (OperationCanceledException)
{
    Console.Error.WriteLine("Game export cancelled.");
    return 130;
}
catch (Exception error) when (error is IOException or ArgumentException or InvalidOperationException or System.Text.Json.JsonException)
{
    Console.Error.WriteLine(error.Message);
    return 1;
}

sealed class ConsoleProgress : IProgress<string>
{
    public void Report(string value) => Console.WriteLine(value);
}
