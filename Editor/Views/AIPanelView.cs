using System.Collections.Specialized;
using SailorEditor.AI;

namespace SailorEditor.Views;

public sealed class AIPanelView : ContentView
{
    readonly AIOperatorService _service;
    readonly Label _contextLabel;
    readonly Label _responseLabel;
    readonly Entry _promptEntry;
    readonly VerticalStackLayout _proposalStack;
    readonly VerticalStackLayout _auditStack;

    public AIPanelView()
    {
        _service = MauiProgram.GetService<AIOperatorService>();

        _contextLabel = new Label { FontSize = 12, Opacity = 0.8 };
        _responseLabel = new Label { FontSize = 12, Opacity = 0.85 };
        _promptEntry = new Entry { Placeholder = "Ask AI to operate the editor. Example: open console" };
        _proposalStack = new VerticalStackLayout { Spacing = 8 };
        _auditStack = new VerticalStackLayout { Spacing = 8 };

        var submitButton = new Button { Text = "Preview Actions" };
        submitButton.Clicked += OnSubmitClicked;

        var header = new Border
        {
            StrokeThickness = 1,
            Margin = new Thickness(8, 8, 8, 4),
            Padding = 12,
            Content = new VerticalStackLayout
            {
                Spacing = 8,
                Children =
                {
                    _contextLabel,
                    _responseLabel,
                    _promptEntry,
                    submitButton,
                }
            }
        };

        var body = new ScrollView
        {
            Margin = new Thickness(8, 4),
            Content = new VerticalStackLayout
            {
                Spacing = 12,
                Children =
                {
                    new Label { Text = "Pending proposals", FontAttributes = FontAttributes.Bold },
                    _proposalStack,
                    new BoxView { HeightRequest = 1, Color = Colors.DimGray },
                    new Label { Text = "Audit trail", FontAttributes = FontAttributes.Bold },
                    _auditStack,
                }
            }
        };
        Grid.SetRow(body, 3);

        var grid = new Grid();
        grid.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        grid.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        grid.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        grid.RowDefinitions.Add(new RowDefinition(GridLength.Star));
        grid.Children.Add(header);
        grid.Children.Add(body);
        Content = grid;

        _service.Proposals.CollectionChanged += OnProposalsChanged;
        _service.AuditTrail.CollectionChanged += OnAuditChanged;
        Unloaded += OnUnloaded;

        RefreshContext();
        RebuildProposals();
        RebuildAudit();
    }

    async void OnSubmitClicked(object? sender, EventArgs e)
    {
        var result = await _service.SubmitPromptAsync(_promptEntry.Text ?? string.Empty);
        _responseLabel.Text = result.Message;
        RefreshContext();
        RebuildProposals();
    }

    void RefreshContext()
    {
        var context = _service.CurrentContext;
        _contextLabel.Text = $"Context • world: {context.ActiveWorldId ?? "none"} • selection: {context.SelectionCount} • panel: {context.ActivePanelId ?? "none"} • viewport: {context.FocusedViewportId ?? "none"}";
    }

    void OnProposalsChanged(object? sender, NotifyCollectionChangedEventArgs e) => MainThread.BeginInvokeOnMainThread(RebuildProposals);

    void OnAuditChanged(object? sender, NotifyCollectionChangedEventArgs e) => MainThread.BeginInvokeOnMainThread(RebuildAudit);

    void RebuildProposals()
    {
        _proposalStack.Children.Clear();

        foreach (var proposal in _service.Proposals.Take(6))
            _proposalStack.Children.Add(CreateProposalCard(proposal));

        if (_proposalStack.Children.Count == 0)
            _proposalStack.Children.Add(new Label { Text = "No proposals yet.", Opacity = 0.7 });
    }

    View CreateProposalCard(AIProposedAction proposal)
    {
        var buttons = new HorizontalStackLayout { Spacing = 8 };
        var execute = new Button { Text = proposal.RequiresApproval ? "Approve + Execute" : "Execute", FontSize = 12 };
        execute.Clicked += async (_, _) =>
        {
            if (proposal.RequiresApproval)
                _service.TryApprove(proposal.Id);
            await _service.ExecuteProposalAsync(proposal.Id, approved: proposal.RequiresApproval);
            RefreshContext();
        };
        buttons.Children.Add(execute);

        if (proposal.RequiresApproval)
        {
            var reject = new Button { Text = "Reject", FontSize = 12 };
            reject.Clicked += (_, _) => _service.Reject(proposal.Id);
            buttons.Children.Add(reject);
        }

        return new Border
        {
            StrokeThickness = 1,
            Padding = 10,
            Content = new VerticalStackLayout
            {
                Spacing = 6,
                Children =
                {
                    new Label { Text = proposal.Title, FontAttributes = FontAttributes.Bold },
                    new Label { Text = $"Safety: {proposal.Safety} • State: {proposal.State}", FontSize = 12, Opacity = 0.75 },
                    new Label { Text = proposal.Summary ?? string.Empty, FontSize = 12, Opacity = 0.85 },
                    new Label { Text = $"Commands: {string.Join(", ", proposal.Commands.Select(x => x.Name))}", FontSize = 12, Opacity = 0.75 },
                    buttons
                }
            }
        };
    }

    void RebuildAudit()
    {
        _auditStack.Children.Clear();

        foreach (var audit in _service.AuditTrail.Take(8))
        {
            var detail = audit.Items.Count == 0
                ? "No commands executed"
                : string.Join(" • ", audit.Items.Select(x => $"{x.CommandName}:{(x.Succeeded ? "ok" : "fail")}"));

            _auditStack.Children.Add(new Border
            {
                StrokeThickness = 1,
                Padding = 10,
                Content = new VerticalStackLayout
                {
                    Spacing = 4,
                    Children =
                    {
                        new Label { Text = audit.Title, FontAttributes = FontAttributes.Bold },
                        new Label { Text = $"{audit.State} • {audit.Safety} • {audit.Timestamp:HH:mm:ss}", FontSize = 12, Opacity = 0.75 },
                        new Label { Text = audit.Summary ?? detail, FontSize = 12, Opacity = 0.85 },
                        new Label { Text = detail, FontSize = 11, Opacity = 0.7 },
                    }
                }
            });
        }

        if (_auditStack.Children.Count == 0)
            _auditStack.Children.Add(new Label { Text = "Nothing executed yet.", Opacity = 0.7 });
    }

    void OnUnloaded(object? sender, EventArgs e)
    {
        _service.Proposals.CollectionChanged -= OnProposalsChanged;
        _service.AuditTrail.CollectionChanged -= OnAuditChanged;
        Unloaded -= OnUnloaded;
    }
}
