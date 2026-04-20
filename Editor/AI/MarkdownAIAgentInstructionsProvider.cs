namespace SailorEditor.AI;

public sealed class MarkdownAIAgentInstructionsProvider : IAIAgentInstructionsProvider
{
    const string RelativePath = "AI/AGENT.md";
    readonly Lazy<string> _instructions = new(LoadInstructions);

    public string Instructions => _instructions.Value;

    static string LoadInstructions()
    {
        foreach (var path in CandidatePaths())
        {
            if (File.Exists(path))
            {
                return File.ReadAllText(path);
            }
        }

        return "Use Sailor editor commands through the AI operator. Require confirmation for world, asset, or file mutations.";
    }

    static IEnumerable<string> CandidatePaths()
    {
        yield return Path.Combine(AppContext.BaseDirectory, RelativePath);

        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var editorPath = Path.Combine(current.FullName, "Editor", RelativePath);
            yield return editorPath;

            var directPath = Path.Combine(current.FullName, RelativePath);
            yield return directPath;

            current = current.Parent;
        }
    }
}
