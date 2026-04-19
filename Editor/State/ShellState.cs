using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using SailorEditor.Layout;
using SailorEditor.Panels;

namespace SailorEditor.State;

public sealed record PanelInstance(
    PanelId PanelId,
    PanelTypeId PanelTypeId,
    string Title,
    PanelRole Role,
    View View,
    bool IsVisible = true,
    string? GroupId = null,
    bool IsFloating = false,
    string? PersistenceKey = null);

public sealed class ShellState : INotifyPropertyChanged
{
    readonly Dictionary<PanelId, PanelInstance> _panelLookup = new();

    EditorLayout? _layout;
    ShellFocusState _focus = new();

    public ObservableCollection<PanelInstance> OpenPanels { get; } = new();

    public EditorLayout? Layout
    {
        get => _layout;
        set => SetField(ref _layout, value);
    }

    public ShellFocusState Focus
    {
        get => _focus;
        private set => SetField(ref _focus, value);
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public bool IsOpen(PanelId panelId) => _panelLookup.ContainsKey(panelId);

    public bool TryGetPanel(PanelId panelId, out PanelInstance instance) => _panelLookup.TryGetValue(panelId, out instance!);

    public PanelInstance OpenPanel(PanelInstance instance, string? groupId = null)
    {
        var normalized = instance with { GroupId = groupId ?? instance.GroupId, IsVisible = true };
        _panelLookup[normalized.PanelId] = normalized;

        var existingIndex = OpenPanels.IndexOf(OpenPanels.FirstOrDefault(x => x.PanelId == normalized.PanelId));
        if (existingIndex >= 0)
            OpenPanels[existingIndex] = normalized;
        else
            OpenPanels.Add(normalized);

        FocusPanel(normalized.PanelId, normalized.GroupId, normalized.Role == PanelRole.Document ? normalized.PanelId : Focus.ActiveDocumentPanelId);
        return normalized;
    }

    public bool ClosePanel(PanelId panelId)
    {
        if (!_panelLookup.Remove(panelId, out var instance))
            return false;

        var existing = OpenPanels.FirstOrDefault(x => x.PanelId == panelId);
        if (existing is not null)
            OpenPanels.Remove(existing);

        if (Focus.FocusedPanelId == panelId)
        {
            var next = OpenPanels.LastOrDefault();
            Focus = Focus with
            {
                FocusedPanelId = next?.PanelId,
                ActiveDocumentPanelId = next?.Role == PanelRole.Document ? next.PanelId : null,
                ActiveTabGroupId = next?.GroupId
            };
        }

        return true;
    }

    public bool HidePanel(PanelId panelId)
    {
        if (!_panelLookup.TryGetValue(panelId, out var existing))
            return false;

        var hidden = existing with { IsVisible = false };
        _panelLookup[panelId] = hidden;
        var index = OpenPanels.IndexOf(OpenPanels.First(x => x.PanelId == panelId));
        OpenPanels[index] = hidden;
        return true;
    }

    public void FocusPanel(PanelId panelId, string? groupId = null, PanelId? activeDocument = null)
    {
        Focus = Focus with
        {
            FocusedPanelId = panelId,
            ActiveTabGroupId = groupId ?? Focus.ActiveTabGroupId,
            ActiveDocumentPanelId = activeDocument ?? Focus.ActiveDocumentPanelId
        };
    }

    public void FocusViewport(string? viewportId, string? selectionOwner = null, string? keyboardInputOwner = null)
    {
        Focus = Focus with
        {
            FocusedViewportId = viewportId,
            SelectionOwner = selectionOwner,
            KeyboardInputOwner = keyboardInputOwner
        };
    }

    bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return false;

        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        return true;
    }
}
