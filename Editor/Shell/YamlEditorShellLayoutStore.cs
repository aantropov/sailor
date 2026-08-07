using SailorEditor.Layout;

namespace SailorEditor.Shell;

public sealed class YamlEditorShellLayoutStore : IEditorShellLayoutStore
{
    readonly string _layoutPath;

    public YamlEditorShellLayoutStore(string? layoutPath = null)
    {
#if MACCATALYST
        var defaultLayoutPath = Path.Combine(
            Microsoft.Maui.Storage.FileSystem.Current.AppDataDirectory,
            "Layouts",
            "editor-shell-layout.yaml");
#else
        var defaultLayoutPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "SailorEditor",
            "Layouts",
            "editor-shell-layout.yaml");
#endif
        _layoutPath = layoutPath ?? defaultLayoutPath;
    }

    public async ValueTask<EditorLayout?> LoadAsync(CancellationToken cancellationToken = default)
    {
        if (!File.Exists(_layoutPath))
            return null;

        var yaml = await File.ReadAllTextAsync(_layoutPath, cancellationToken);
        return LayoutYamlSerializer.Deserialize(yaml);
    }

    public async ValueTask SaveAsync(EditorLayout layout, CancellationToken cancellationToken = default)
    {
        var directory = Path.GetDirectoryName(_layoutPath);
        if (!string.IsNullOrWhiteSpace(directory))
            Directory.CreateDirectory(directory);

        var yaml = LayoutYamlSerializer.Serialize(layout);
        await File.WriteAllTextAsync(_layoutPath, yaml, cancellationToken);
    }
}
