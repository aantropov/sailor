using System.ComponentModel;
using System.Runtime.CompilerServices;
using SailorEditor.Layout;
using SailorEditor.Panels;
using SailorEditor.State;

namespace SailorEditor.Shell;

public sealed class EditorShellHost : IEditorShellHost, INotifyPropertyChanged
{
    readonly PanelRegistry _registry;
    readonly IEditorShellLayoutStore _layoutStore;

    string _statusText = "Ready";

    public EditorShellHost(PanelRegistry registry, IEditorShellLayoutStore layoutStore, ShellState state)
    {
        _registry = registry;
        _layoutStore = layoutStore;
        State = state;
    }

    public ShellState State { get; }

    public ShellFocusState Focus => State.Focus;

    public EditorLayout? CurrentLayout => State.Layout;

    public IReadOnlyCollection<PanelDescriptor> PanelDescriptors => _registry.GetAllDescriptors();

    public string StatusText
    {
        get => _statusText;
        private set => SetField(ref _statusText, value);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public async Task InitializeAsync(CancellationToken cancellationToken = default)
    {
        EditorLayout layout;
        try
        {
            layout = await _layoutStore.LoadAsync(cancellationToken) ?? LayoutOperations.CreateDefaultLayout();
        }
        catch
        {
            layout = LayoutOperations.CreateDefaultLayout();
            StatusText = "Layout restore failed. Loaded default layout.";
        }

        ApplyLayout(layout);
    }

    public void ApplyLayout(EditorLayout layout)
    {
        layout = layout with { Root = new LayoutRoot(LayoutOperations.CleanupEmptyGroups(layout.Root.Content)) };
        State.Layout = layout;
        RebuildOpenPanels(layout.Root.Content);
        RaisePropertyChanged(nameof(CurrentLayout));
        RaisePropertyChanged(nameof(State));
        StatusText = $"Layout loaded • {State.OpenPanels.Count} panels";
    }

    public async ValueTask OpenPanelAsync(PanelTypeId panelTypeId, CancellationToken cancellationToken = default)
        => await OpenPanelInGroupAsync(panelTypeId, null, cancellationToken);

    public async ValueTask OpenPanelInGroupAsync(PanelTypeId panelTypeId, string? targetGroupId, CancellationToken cancellationToken = default)
    {
        if (!_registry.TryGetDescriptor(panelTypeId, out var descriptor))
            return;

        var existing = State.OpenPanels.FirstOrDefault(x => x.PanelTypeId == panelTypeId);
        if (existing is not null)
        {
            if (!string.IsNullOrWhiteSpace(targetGroupId) && !string.Equals(existing.GroupId, targetGroupId, StringComparison.Ordinal))
            {
                await MovePanelToGroupAsync(existing.PanelId, targetGroupId!, cancellationToken);
                return;
            }

            State.OpenPanel(existing with { IsVisible = true }, existing.GroupId);
            await FocusPanelAsync(existing.PanelId, cancellationToken);
            return;
        }

        var reference = new PanelReference(PanelId.New(), panelTypeId);
        targetGroupId ??= descriptor.DefaultDockPreference.TargetGroupId ?? GuessDefaultGroupId(panelTypeId);
        var updatedRoot = CurrentLayout is null
            ? LayoutOperations.CreateDefaultLayout().Root.Content
            : LayoutOperations.InsertTabbed(CurrentLayout.Root.Content, targetGroupId, reference);

        var layout = (CurrentLayout ?? LayoutOperations.CreateDefaultLayout()) with { Root = new LayoutRoot(updatedRoot) };
        ApplyLayout(layout);
        await SaveLayoutAsync(cancellationToken);
        await FocusPanelAsync(reference.PanelId, cancellationToken);
        StatusText = $"Opened {descriptor.Title} • dock: {targetGroupId}";
    }

    public ValueTask FocusPanelAsync(PanelId panelId, CancellationToken cancellationToken = default)
        => FocusPanelAsync(panelId, activateLayout: true);

    public ValueTask FocusPanelLocalAsync(PanelId panelId)
        => FocusPanelAsync(panelId, activateLayout: false);

    ValueTask FocusPanelAsync(PanelId panelId, bool activateLayout)
    {
        if (State.TryGetPanel(panelId, out var instance))
        {
            State.FocusPanel(panelId, instance.GroupId, instance.Role == PanelRole.Document ? panelId : null);
            if (activateLayout)
                ActivatePanelInLayout(panelId);
            StatusText = $"Focused {instance.Title}";
        }

        return ValueTask.CompletedTask;
    }

    public async Task ClosePanelAsync(PanelId panelId, CancellationToken cancellationToken = default)
    {
        if (CurrentLayout is null)
            return;

        var updated = CurrentLayout with { Root = new LayoutRoot(LayoutOperations.HidePanel(CurrentLayout.Root.Content, panelId)) };
        ApplyLayout(updated);
        await SaveLayoutAsync(cancellationToken);
    }

    public async Task MovePanelToGroupAsync(PanelId panelId, string targetGroupId, CancellationToken cancellationToken = default)
    {
        if (CurrentLayout is null || string.IsNullOrWhiteSpace(targetGroupId))
            return;

        var updatedRoot = LayoutOperations.MovePanelToGroup(CurrentLayout.Root.Content, panelId, targetGroupId);
        if (ReferenceEquals(updatedRoot, CurrentLayout.Root.Content))
            return;

        var updated = CurrentLayout with { Root = new LayoutRoot(updatedRoot) };
        ApplyLayout(updated);
        await SaveLayoutAsync(cancellationToken);
        await FocusPanelAsync(panelId, cancellationToken);

        if (State.TryGetPanel(panelId, out var instance))
            StatusText = $"Moved {instance.Title} • dock: {targetGroupId}";
    }

    public async Task ResizeSplitAsync(IReadOnlyList<int> splitPath, int leadingIndex, double delta, CancellationToken cancellationToken = default)
    {
        if (CurrentLayout is null || Math.Abs(delta) < double.Epsilon)
            return;

        var updatedRoot = LayoutOperations.ResizeAtPath(CurrentLayout.Root.Content, splitPath, leadingIndex, delta);
        var updatedLayout = CurrentLayout with { Root = new LayoutRoot(updatedRoot) };
        ApplyLayout(updatedLayout);
        await SaveLayoutAsync(cancellationToken);
        StatusText = "Layout resized";
    }

    public async Task SaveLayoutAsync(CancellationToken cancellationToken = default)
    {
        if (CurrentLayout is null)
            return;

        await _layoutStore.SaveAsync(CurrentLayout, cancellationToken);
        StatusText = "Layout saved";
    }

    public async Task ResetLayoutAsync(CancellationToken cancellationToken = default)
    {
        ApplyLayout(LayoutOperations.CreateDefaultLayout());
        await SaveLayoutAsync(cancellationToken);
        StatusText = "Layout reset";
    }

    void RebuildOpenPanels(LayoutNode root)
    {
        State.ResetPanels();
        Traverse(root);
    }

    void ActivatePanelInLayout(PanelId panelId)
    {
        if (CurrentLayout is null)
            return;

        var updatedRoot = LayoutOperations.ActivatePanel(CurrentLayout.Root.Content, panelId);
        if (Equals(updatedRoot, CurrentLayout.Root.Content))
            return;

        State.Layout = CurrentLayout with { Root = new LayoutRoot(updatedRoot) };
        RaisePropertyChanged(nameof(CurrentLayout));
    }

    void Traverse(LayoutNode node)
    {
        switch (node)
        {
            case TabGroupNode tabs:
                foreach (var panel in tabs.Panels)
                {
                    if (!_registry.TryGetDescriptor(panel.PanelTypeId, out var descriptor))
                        continue;

                    var view = descriptor.Factory?.Invoke() as View ?? new Label { Text = descriptor.Title };
                    State.OpenPanel(new PanelInstance(panel.PanelId, panel.PanelTypeId, panel.TitleOverride ?? descriptor.Title, descriptor.Role, view, true, tabs.GroupId, false, panel.PersistenceKey), tabs.GroupId);
                }

                if (tabs.ActivePanelId is { } active)
                    State.FocusPanel(active, tabs.GroupId, State.OpenPanels.FirstOrDefault(x => x.PanelId == active)?.Role == PanelRole.Document ? active : null);
                break;

            case SplitNode split:
                foreach (var child in split.Children)
                    Traverse(child);
                break;
        }
    }

    static string GuessDefaultGroupId(PanelTypeId panelTypeId) => panelTypeId.Value switch
    {
        "Scene" => "center-docs",
        "Console" => "bottom-console",
        "Inspector" => "right-inspector",
        "Hierarchy" => "left-bottom",
        "Content" => "left-top",
        "EditorPerformance" => "bottom-console",
        _ => "right-inspector"
    };

    void RaisePropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return false;

        field = value;
        RaisePropertyChanged(propertyName);
        return true;
    }
}
