using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.Globalization;
using SailorEngine;
using SailorEditor.Utility;
using Microsoft.Maui.Controls;

namespace SailorEditor.Controls;

public class FileIdDragAndDropBehaviour : Behavior<Label>
{
    private DragGestureRecognizer _dragGestureRecognizer;
    private DropGestureRecognizer _dropGestureRecognizer;

    private Label _associatedLabel;

    public static readonly BindableProperty BoundPropertyProperty =
        BindableProperty.Create(nameof(BoundProperty), typeof(object), typeof(FileIdDragAndDropBehaviour), null, BindingMode.TwoWay);

    public object BoundProperty
    {
        get => GetValue(BoundPropertyProperty);
        set => SetValue(BoundPropertyProperty, value);
    }

    public static readonly BindableProperty SupportedTypeProperty =
        BindableProperty.Create(nameof(SupportedType), typeof(Type), typeof(FileIdDragAndDropBehaviour), null);

    public Type SupportedType
    {
        get => (Type)GetValue(SupportedTypeProperty);
        set => SetValue(SupportedTypeProperty, value);
    }

    protected override void OnAttachedTo(Label bindable)
    {
        base.OnAttachedTo(bindable);

        _associatedLabel = bindable;
        _associatedLabel.BindingContextChanged += OnBindingContextChanged;

        BindingContext = _associatedLabel.BindingContext;

        _dragGestureRecognizer = new DragGestureRecognizer();
        _dragGestureRecognizer.DragStarting += OnDragStarting;

        _dropGestureRecognizer = new DropGestureRecognizer();
        _dropGestureRecognizer.Drop += OnDrop;

        bindable.GestureRecognizers.Add(_dragGestureRecognizer);
        bindable.GestureRecognizers.Add(_dropGestureRecognizer);
    }

    protected override void OnDetachingFrom(Label bindable)
    {
        base.OnDetachingFrom(bindable);
        _associatedLabel.BindingContextChanged -= OnBindingContextChanged;
        _associatedLabel = null;
    }

    private void OnBindingContextChanged(object sender, System.EventArgs e)
    {
        var bindingContext = _associatedLabel.BindingContext;
        BindingContext = _associatedLabel.BindingContext;
    }

    private void OnDragStarting(object sender, DragStartingEventArgs e)
    {
        if (BoundProperty != null)
        {
            e.Data.Properties.Add("DragItem", BoundProperty);
        }
    }

    private void OnDrop(object sender, DropEventArgs e)
    {
        if (sender != this && e.Data.Properties.TryGetValue("DragItem", out var droppedItem))
        {
            FileId fileId = droppedItem as FileId ?? (droppedItem as AssetFile).FileId;

            if (fileId != null)
            {
                var assetService = MauiProgram.GetService<AssetsService>();

                if (assetService.Assets.TryGetValue(fileId, out var asset))
                {
                    if (SupportedType == null || SupportedType.IsInstanceOfType(asset))
                    {
                        BoundProperty = fileId;
                    }
                }
            }
        }

        e.Handled = true;
    }
}