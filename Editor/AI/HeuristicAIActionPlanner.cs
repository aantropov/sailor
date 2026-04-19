using SailorEditor.Commands;
using SailorEditor.Panels;
using SailorEditor.Shell;

namespace SailorEditor.AI;

public sealed class HeuristicAIActionPlanner : IAIActionPlanner
{
    readonly EditorShellHost _shellHost;

    public HeuristicAIActionPlanner(EditorShellHost shellHost)
    {
        _shellHost = shellHost;
    }

    public Task<AIPlanResponse> PlanAsync(string prompt, AIEditorContextSnapshot context, CancellationToken cancellationToken = default)
    {
        var normalized = prompt.Trim().ToLowerInvariant();
        var actions = new List<AIProposedAction>();
        var response = $"Context: world={context.ActiveWorldId ?? "none"}, selection={context.SelectionCount}, panel={context.ActivePanelId ?? "none"}.";

        if (TryMatchPanel(normalized, out var panelType, out var panelTitle))
        {
            actions.Add(new AIProposedAction(
                Guid.NewGuid().ToString("N"),
                $"Open {panelTitle}",
                new IEditorCommand[] { new OpenPanelCommand(_shellHost, panelType, $"Opened {panelTitle}") },
                AIActionSafety.SafeAutoExecute,
                $"Open the {panelTitle} panel in the shell."));

            response += $" I prepared a safe shell action to open {panelTitle}.";
        }

        if (normalized.Contains("create object") || normalized.Contains("create game object") || normalized.Contains("create gameobject"))
        {
            actions.Add(new AIProposedAction(
                Guid.NewGuid().ToString("N"),
                "Create GameObject",
                new IEditorCommand[] { new CreateGameObjectCommand() },
                AIActionSafety.ConfirmRequired,
                "Create a new GameObject through the standard command path."));

            response += " I also prepared a confirm-required world mutation to create a GameObject.";
        }

        if (actions.Count == 0)
        {
            response += " I can currently preview shell open/focus actions and simple object creation through commands. Try: 'open console' or 'create game object'.";
        }

        return Task.FromResult(new AIPlanResponse(prompt, response, context, actions));
    }

    static bool TryMatchPanel(string prompt, out PanelTypeId panelTypeId, out string panelTitle)
    {
        foreach (var (needle, type, title) in PanelMappings)
        {
            if (!prompt.Contains(needle))
                continue;

            panelTypeId = type;
            panelTitle = title;
            return true;
        }

        panelTypeId = default;
        panelTitle = string.Empty;
        return false;
    }

    static readonly (string Needle, PanelTypeId Type, string Title)[] PanelMappings =
    [
        ("open ai", KnownPanelTypes.AI, "AI"),
        ("open console", KnownPanelTypes.Console, "Console"),
        ("open inspector", KnownPanelTypes.Inspector, "Inspector"),
        ("open hierarchy", KnownPanelTypes.Hierarchy, "Hierarchy"),
        ("open content", KnownPanelTypes.Content, "Content"),
        ("open project", KnownPanelTypes.Content, "Content"),
        ("open settings", KnownPanelTypes.Settings, "Settings"),
        ("open scene", KnownPanelTypes.Scene, "Scene"),
    ];
}
