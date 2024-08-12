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

    //public static readonly BindableProperty SupportedTypesProperty =
    //    BindableProperty.Create(nameof(SupportedTypes), typeof(IList<string>), typeof(FileIdDragAndDropBehaviour), new List<string>());

    //public IList<string> SupportedTypes
    //{
    //    get => (IList<string>)GetValue(SupportedTypesProperty);
    //    set => SetValue(SupportedTypesProperty, value);
    //}

    //private List<Type> _supportedTypeList;

    protected override void OnAttachedTo(Label bindable)
    {
        base.OnAttachedTo(bindable);

        _associatedLabel = bindable;
        _associatedLabel.BindingContextChanged += OnBindingContextChanged;

        //_supportedTypeList = SupportedTypes.Select(typeName => Type.GetType(typeName)).ToList();

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
                    //     if (_supportedTypeList.Any(type => type.IsInstanceOfType(asset)))
                    {
                        BoundProperty = fileId;
                    }
                }
            }
        }

        e.Handled = true;
    }
}