using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor.Content;

namespace SailorEditor.ViewModels;

public partial class RenderTextureFile : TextureFile
{
    [ObservableProperty]
    int renderWidth = 512;

    [ObservableProperty]
    int renderHeight = 512;

    [ObservableProperty]
    string renderFormat = "R8G8B8A8_UNORM";

    public IReadOnlyList<string> RenderFormats => RenderTextureDescriptor.SupportedFormats;

    public override Task PrepareInspectorResources()
    {
        return Task.CompletedTask;
    }

    public override Task<bool> LoadDependentResources(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        LoadRuntimeDataWithoutDirtyTracking(() => IsLoaded = true);
        return Task.FromResult(true);
    }

    public override async Task Revert()
    {
        await base.Revert();

        try
        {
            RunWithoutDirtyTracking(() =>
            {
                var descriptor = RenderTextureDescriptor.Parse(
                    File.ReadAllText(Asset.FullName));

                RenderWidth = descriptor.Width;
                RenderHeight = descriptor.Height;
                RenderFormat = descriptor.Format;

                DisplayName = Asset.Name;
                IsLoaded = false;
            });
        }
        catch (Exception ex)
        {
            SetLoadError(ex);
        }

        ResetDirtyState();
    }

    public override async Task Save()
    {
        EnsureWritable();

        var descriptor = new RenderTextureDescriptor(
            RenderWidth, RenderHeight, RenderFormat);

        var sourceText = descriptor.Serialize();

        await File.WriteAllTextAsync(Asset.FullName, sourceText);

        await base.Save();
    }
}
