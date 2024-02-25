using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using System.ComponentModel;
using System.Globalization;

public class AssetUIDClickable : Behavior<Label>
{
    string bindingPath;

    public AssetUIDClickable(string property)
    {
        bindingPath = property;
    }
    protected override void OnAttachedTo(Label bindable)
    {
        base.OnAttachedTo(bindable);
        var tapGestureRecognizer = new TapGestureRecognizer();
        tapGestureRecognizer.Tapped += OnLabelTapped;
        bindable.GestureRecognizers.Add(tapGestureRecognizer);
    }

    protected override void OnDetachingFrom(Label bindable)
    {
        base.OnDetachingFrom(bindable);
        if (bindable.GestureRecognizers.FirstOrDefault() is TapGestureRecognizer tapGestureRecognizer)
        {
            tapGestureRecognizer.Tapped -= OnLabelTapped;
            bindable.GestureRecognizers.Remove(tapGestureRecognizer);
        }
    }

    private void OnLabelTapped(object sender, EventArgs e)
    {
        var property = (sender as Label).BindingContext.GetType().GetProperty(bindingPath);
        if (property != null)
        {
            var uid = property.GetValue((sender as Label).BindingContext);

            MauiProgram.GetService<SelectionService>().OnSelectAsset(MauiProgram.GetService<AssetsService>().Assets[uid.ToString()]);
        }
    }
}