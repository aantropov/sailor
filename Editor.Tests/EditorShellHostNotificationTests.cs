using SailorEditor.Layout;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.State;

namespace Editor.Tests;

public sealed class EditorShellHostNotificationTests
{
    [Fact]
    public void ApplyLayout_RaisesCurrentLayoutChanged()
    {
        var shellHost = CreateHost();
        var notifications = new List<string?>();
        shellHost.PropertyChanged += (_, e) => notifications.Add(e.PropertyName);

        shellHost.ApplyLayout(LayoutOperations.CreateDefaultLayout());

        Assert.Contains(nameof(EditorShellHost.CurrentLayout), notifications);
    }

    [Fact]
    public async Task FocusPanelAsync_RaisesCurrentLayoutChangedForShellRefresh()
    {
        var shellHost = CreateHost();
        shellHost.ApplyLayout(LayoutOperations.CreateDefaultLayout());
        var targetPanelId = shellHost.State.OpenPanels.First(x => x.PanelTypeId == KnownPanelTypes.Console).PanelId;
        var notifications = new List<string?>();
        shellHost.PropertyChanged += (_, e) => notifications.Add(e.PropertyName);

        await shellHost.FocusPanelAsync(targetPanelId);

        Assert.Contains(nameof(EditorShellHost.CurrentLayout), notifications);
    }

    static EditorShellHost CreateHost() => new(new PanelRegistry(), new InMemoryLayoutStore(), new ShellState());

    sealed class InMemoryLayoutStore : IEditorShellLayoutStore
    {
        public ValueTask<EditorLayout?> LoadAsync(CancellationToken cancellationToken = default) => ValueTask.FromResult<EditorLayout?>(null);

        public ValueTask SaveAsync(EditorLayout layout, CancellationToken cancellationToken = default) => ValueTask.CompletedTask;
    }
}
