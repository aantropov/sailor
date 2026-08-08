using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using SailorEngine;
using SailorEditor.Utility;

namespace SailorEditor.Controls;

public class FileIdSelectBehavior : Behavior<Label>
{
    private Label _associatedLabel;
    private AssetsService _assetsService;
    private TapGestureRecognizer _tapGestureRecognizer;

    public static readonly BindableProperty BoundPropertyProperty =
        BindableProperty.Create(
            nameof(BoundProperty),
            typeof(object),
            typeof(FileIdSelectBehavior),
            null,
            BindingMode.TwoWay,
            propertyChanged: OnBoundPropertyChanged);

    public object BoundProperty
    {
        get => GetValue(BoundPropertyProperty);
        set => SetValue(BoundPropertyProperty, value);
    }

    protected override void OnAttachedTo(Label bindable)
    {
        base.OnAttachedTo(bindable);

        _associatedLabel = bindable;
        _associatedLabel.BindingContextChanged += OnBindingContextChanged;

        BindingContext = _associatedLabel.BindingContext;
        _assetsService = MauiProgram.GetService<AssetsService>();
        _assetsService.Changed += OnAssetsChanged;

        _tapGestureRecognizer = new TapGestureRecognizer();
        _tapGestureRecognizer.Tapped += OnLabelTapped;
        bindable.GestureRecognizers.Add(_tapGestureRecognizer);
        RefreshLabel();
    }

    protected override void OnDetachingFrom(Label bindable)
    {
        base.OnDetachingFrom(bindable);

        if (_assetsService is not null)
        {
            _assetsService.Changed -= OnAssetsChanged;
            _assetsService = null;
        }
        if (_tapGestureRecognizer is not null)
        {
            _tapGestureRecognizer.Tapped -= OnLabelTapped;
            bindable.GestureRecognizers.Remove(_tapGestureRecognizer);
            _tapGestureRecognizer = null;
        }
        _associatedLabel.BindingContextChanged -= OnBindingContextChanged;
        _associatedLabel = null;
    }

    private async void OnLabelTapped(object sender, EventArgs e)
    {
        try
        {
            await MauiProgram.GetService<SelectionService>()
                .NavigateToReferenceAsync(BoundProperty);
        }
        catch (Exception exception)
        {
            Console.WriteLine(
                $"[FileIdSelectBehavior] Failed to navigate to reference: {exception.Message}");
        }
    }

    private void OnBindingContextChanged(object sender, System.EventArgs e)
    {
        BindingContext = _associatedLabel.BindingContext;
        RefreshLabel();
    }

    static void OnBoundPropertyChanged(
        BindableObject bindable,
        object oldValue,
        object newValue)
        => ((FileIdSelectBehavior)bindable).RefreshLabel();

    void OnAssetsChanged()
    {
        if (MainThread.IsMainThread)
        {
            RefreshLabel();
        }
        else
        {
            MainThread.BeginInvokeOnMainThread(RefreshLabel);
        }
    }

    void RefreshLabel()
    {
        if (_associatedLabel is null)
        {
            return;
        }

        var fileId = BoundProperty as FileId ??
            (BoundProperty as Observable<FileId>)?.Value;
        if (fileId is null || fileId.IsEmpty())
        {
            _associatedLabel.Text = FileId.NullFileId;
            return;
        }

        _associatedLabel.Text =
            _assetsService?.Assets.TryGetValue(fileId, out var asset) == true
                ? asset.DisplayName
                : fileId.Value;
    }
}
