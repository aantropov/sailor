using SailorEditor.Commands;
using SailorEditor.Shell;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEditor.Workflow;
using SailorEngine;
using YamlDotNet.Serialization;

namespace SailorEditor.Services;

internal sealed class ComponentClipboardService
{
    static readonly HashSet<string> IdentityProperties = new(
        ["instanceId", "fileId"],
        StringComparer.Ordinal);

    string? copiedValuesYaml;

    public async Task CopyValuesAsync(
        Component component,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(component);
        cancellationToken.ThrowIfCancellationRequested();
        copiedValuesYaml = EditorYaml.SerializeComponent(component);

        try
        {
            await Clipboard.Default.SetTextAsync(copiedValuesYaml);
        }
        catch (Exception ex)
        {
            // Copy/Paste Values is an editor operation first. Keep it working
            // even if the platform clipboard is temporarily unavailable.
            Console.Error.WriteLine($"Could not publish component values to the system clipboard: {ex}");
        }

        Console.WriteLine($"Copied values for {component.Typename?.Name}.");
        MauiProgram.GetService<EditorShellHost>()
            .SetStatus($"Copied {component.Typename?.Name} values");
    }

    public async Task PasteValuesAsync(
        Component component,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(component);
        var clipboardText = copiedValuesYaml;
        if (string.IsNullOrWhiteSpace(clipboardText))
            clipboardText = await Clipboard.Default.GetTextAsync();
        cancellationToken.ThrowIfCancellationRequested();
        if (string.IsNullOrWhiteSpace(clipboardText))
            throw new InvalidDataException("The clipboard does not contain component values.");

        var beforeYaml = EditorYaml.SerializeComponent(component);
        var afterYaml = MergeValues(component, beforeYaml, clipboardText);
        if (string.Equals(beforeYaml, afterYaml, StringComparison.Ordinal))
        {
            MauiProgram.GetService<EditorShellHost>()
                .SetStatus($"{component.Typename?.Name} values already match");
            return;
        }

        var dispatcher = MauiProgram.GetService<ICommandDispatcher>();
        var contextProvider = MauiProgram.GetService<IActionContextProvider>();
        var result = await dispatcher.DispatchAsync(
            new UpdateComponentCommand(
                component,
                beforeYaml,
                afterYaml,
                $"Paste {component.Typename?.Name} values"),
            contextProvider.GetCurrentContext(
                new CommandOrigin(
                    CommandOriginKind.Menu,
                    nameof(ComponentClipboardService))),
            cancellationToken);
        if (!result.Succeeded)
        {
            throw new InvalidOperationException(
                result.Message ?? "Component values could not be applied.");
        }

        Console.WriteLine($"Pasted values into {component.Typename?.Name}.");
        MauiProgram.GetService<EditorShellHost>()
            .SetStatus($"Pasted {component.Typename?.Name} values");
    }

    internal static string MergeValues(
        Component target,
        string targetYaml,
        string clipboardYaml)
    {
        if (target.Typename is null || string.IsNullOrWhiteSpace(target.Typename.Name))
            throw new InvalidDataException("The target component type is unavailable.");

        var deserializer = SerializationUtils.CreateDeserializerBuilder()
            .WithTypeConverter(new ComponentYamlConverter())
            .Build();
        var source = deserializer.Deserialize<Component>(clipboardYaml) ??
            throw new InvalidDataException("The clipboard does not contain component values.");
        var destination = deserializer.Deserialize<Component>(targetYaml) ??
            throw new InvalidDataException("The target component values are unavailable.");

        if (!string.Equals(
                source.Typename?.Name,
                target.Typename.Name,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                $"Cannot paste values from '{source.Typename?.Name}' into '{target.Typename.Name}'.");
        }

        foreach (var property in source.OverrideProperties)
        {
            if (IdentityProperties.Contains(property.Key) ||
                EditorComponentPropertyContract.Classify(
                    property.Key,
                    target.Typename.Properties,
                    target.Typename.ReadOnlyProperties) !=
                    EditorComponentPropertyAccess.Writable)
            {
                continue;
            }

            destination.OverrideProperties[property.Key] = property.Value;
        }

        return EditorYaml.SerializeComponent(destination);
    }
}
