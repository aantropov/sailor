using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEngine;

namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Skip)]
public partial class ModelFileTemplate : DataTemplate
{
    public ModelFileTemplate()
    {
        InitializeComponent();
    }

    void OnEntryCompleted(object sender, EventArgs e) => ((Entry)sender).Unfocus();

    async void OnMaterialPreviewTapped(object sender, TappedEventArgs args)
    {
        if (sender is BindableObject { BindingContext: Observable<FileId> material })
        {
            await MauiProgram.GetService<SelectionService>().NavigateToReferenceAsync(material.Value);
        }
    }
}
