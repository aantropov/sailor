#nullable enable

namespace SailorEngine;

public sealed class EditorComponentYamlContract
{
    public string Typename { get; set; } = string.Empty;
    public Dictionary<string, object> OverrideProperties { get; set; } = [];
}
